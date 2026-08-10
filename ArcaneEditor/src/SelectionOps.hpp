// Arcane/ArcaneEditor/src/SelectionOps.hpp
#pragma once

// Edit-menu selection collectors (spec II.A). Registry access stays here, on
// the app side -- SelectionContext deliberately has none. ImGui-free so
// ArcaneTests exercises them headless.

#include "SelectionContext.hpp"

#include <Arcane/Scene/SceneResources.hpp>

#include <Astra/Registry/Registry.hpp>

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
