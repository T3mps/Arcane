#pragma once

#include <Astra/Entity/Entity.hpp>

namespace Arcane::Editor
{
    // The one selected-entity source of truth, shared by hierarchy, inspector,
    // and viewport pick. The GPU hit-proxy pick returns the single front-most
    // entity (no occlusion cycling in v1 -- see the entity-id picking spec S5/S7),
    // so no hit-stack state is kept here.
    struct SelectionContext
    {
        using EntityT = Astra::Entity;

        Astra::Entity selected = Astra::Entity::Invalid();

        [[nodiscard]] bool HasSelection() const noexcept { return selected.IsValid(); }
        void Select(Astra::Entity e) noexcept { selected = e; }
        void Clear() noexcept { selected = Astra::Entity::Invalid(); }
    };
}
