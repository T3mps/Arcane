#pragma once

#include <Astra/Entity/Entity.hpp>

#include <vector>

namespace Astra { class Registry; }

namespace Arcane::Editor
{
    // Every live entity in the registry, in EntityManager iteration order. A pure
    // read -- the hierarchy panel's data source. (No Name component exists yet, so
    // the panel labels rows by entity id.)
    std::vector<Astra::Entity> CollectEntities(Astra::Registry& registry);
}
