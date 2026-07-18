#pragma once

#include <Astra/Entity/Entity.hpp>

#include <cstddef>
#include <vector>

namespace Grimoire
{
    // The one selected-entity source of truth, shared by hierarchy, inspector,
    // and viewport pick. pickCandidates/pickCycle hold the last click's sorted
    // hit stack so alt-click can cycle (Task 7).
    struct SelectionContext
    {
        using EntityT = Astra::Entity;

        Astra::Entity              selected = Astra::Entity::Invalid();
        std::vector<Astra::Entity> pickCandidates;
        std::size_t                pickCycle = 0;

        [[nodiscard]] bool HasSelection() const noexcept { return selected.IsValid(); }
        void Select(Astra::Entity e) noexcept { selected = e; }
        void Clear() noexcept { selected = Astra::Entity::Invalid(); pickCandidates.clear(); pickCycle = 0; }
    };
}
