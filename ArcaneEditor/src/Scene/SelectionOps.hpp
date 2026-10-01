// Arcane/ArcaneEditor/src/SelectionOps.hpp
#pragma once

// Edit-menu selection collectors (spec II.A). Registry access stays here, on
// the app side -- SelectionContext deliberately has none. ImGui-free so
// ArcaneTests exercises them headless.

#include "Scene/SelectionContext.hpp"

#include <Arcane/Edit/EntityOps.hpp>
#include <Arcane/Scene/SceneResources.hpp>

#include <Astra/Registry/Registry.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace Arcane::Editor
{
    // Every entity under SceneRoot in scene walk order (BFS pre-order -- NOT
    // the Outliner's current sort, which is panel-local state). The root
    // itself is EXCLUDED: it is the scene container, and a Select All that
    // included it would hand Delete/Cut the whole scene. Empty when the
    // registry has no SceneRoot.
    [[nodiscard]] inline std::vector<Astra::Entity>
    CollectSceneEntities(Astra::Registry& reg)
    {
        std::vector<Astra::Entity> out;
        const Arcane::SceneRoot* root = reg.GetResource<Arcane::SceneRoot>();
        if (!root)
            return out;
        reg.GetRelations(root->entity).ForEachDescendant(
            [&](Astra::Entity e, std::size_t) { out.push_back(e); });
        return out;
    }

    // The root guard for the structural verbs (node page + editor upgrades
    // s3.1): Delete, Cut, Copy and Duplicate call this, and a drag's `moving`
    // set goes through it too. A mixed selection drops the root and the verb
    // proceeds over the rest; a root-only selection comes back empty and the
    // verb is a no-op (its menu item is greyed with SceneRootRefusal). Order
    // is preserved; dead entries are kept (each verb already tolerates them).
    [[nodiscard]] inline std::vector<Astra::Entity>
    SelectionWithoutSceneRoot(const Astra::Registry& reg, std::span<const Astra::Entity> entities)
    {
        std::vector<Astra::Entity> out;
        out.reserve(entities.size());
        for (Astra::Entity e : entities)
            if (!Arcane::Edit::IsSceneRoot(reg, e))
                out.push_back(e);
        return out;
    }

    // Non-empty and nothing but the root: the state that greys the verbs.
    [[nodiscard]] inline bool
    IsSceneRootOnly(const Astra::Registry& reg, std::span<const Astra::Entity> entities)
    {
        return !entities.empty() && SelectionWithoutSceneRoot(reg, entities).empty();
    }

    enum class SceneRootVerb : std::uint8_t { Delete, Cut, Copy, Duplicate };

    // The disabled item's reason (drafting pick, 9.28.7).
    [[nodiscard]] constexpr const char* SceneRootRefusal(SceneRootVerb verb) noexcept
    {
        switch (verb)
        {
            case SceneRootVerb::Delete:    return "The scene root can't be deleted";
            case SceneRootVerb::Cut:       return "The scene root can't be cut";
            case SceneRootVerb::Copy:      return "The scene root can't be copied";
            case SceneRootVerb::Duplicate: return "The scene root can't be duplicated";
        }
        return "";
    }

    // The entries of `all` not currently selected, order preserved.
    [[nodiscard]] inline std::vector<Astra::Entity>
    InvertSelectionSet(const std::vector<Astra::Entity>& all, const SelectionContext& sel)
    {
        std::vector<Astra::Entity> out;
        out.reserve(all.size());
        for (Astra::Entity e : all)
            if (!sel.Contains(e))
                out.push_back(e);
        return out;
    }
}
