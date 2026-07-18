#pragma once

#include <Arcane/Base/Api.hpp>

#include <Astra/Entity/Entity.hpp>

#include <glm/glm.hpp>

#include <vector>

namespace Astra { class Registry; }

namespace Arcane
{
    // Collect every entity whose (WorldTransform, SpriteRenderer) oriented box
    // contains `worldPoint`, sorted front-most first by (sortingLayer, orderInLayer)
    // descending (stable among ties). front() is the top pick; the whole list drives
    // alt-click cycling. Pure read over the registry -- headless, deterministic.
    ARCANE_API std::vector<Astra::Entity> PickEntitiesAt(Astra::Registry& registry,
                                                         glm::vec2 worldPoint);
}
