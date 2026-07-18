#include <Arcane/Scene/EntityPick.hpp>

#include <Arcane/Scene/Components.hpp>

#include <Astra/Registry/Registry.hpp>

#include <algorithm>
#include <cmath>

namespace Arcane
{
    std::vector<Astra::Entity> PickEntitiesAt(Astra::Registry& registry, glm::vec2 worldPoint)
    {
        struct Hit { Astra::Entity e; int32_t layer; int32_t order; };
        std::vector<Hit> hits;

        auto view = registry.CreateView<WorldTransform, SpriteRenderer>();
        view.ForEach([&](Astra::Entity e, WorldTransform& xf, SpriteRenderer& sp)
        {
            // world -> sprite-local via the inverse 2D homogeneous world matrix.
            const glm::vec3 local = glm::inverse(xf.matrix) * glm::vec3(worldPoint, 1.0f);
            const glm::vec2 half = sp.size * 0.5f;
            if (std::abs(local.x) <= half.x && std::abs(local.y) <= half.y)
                hits.push_back({ e, sp.sortingLayer, sp.orderInLayer });
        });

        std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b)
        {
            if (a.layer != b.layer) return a.layer > b.layer;   // higher layer = front-most
            return a.order > b.order;
        });

        std::vector<Astra::Entity> out;
        out.reserve(hits.size());
        for (const Hit& h : hits) out.push_back(h.e);
        return out;
    }
}
