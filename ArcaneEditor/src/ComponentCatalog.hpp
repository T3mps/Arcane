#pragma once

// Add/Remove Component core (Outliner slice 4): the pure, headless-tested half
// of the Inspector's component catalog. The ImGui popup that draws it lives in
// EditorPanels.cpp -- same split as EntityList.hpp / BuildOutlinerRows.

#include <Astra/Entity/Entity.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Astra { class Registry; struct ComponentDescriptor; }

namespace Arcane::Editor
{
    // Components whose lifecycle something OTHER than the generic component UI
    // owns, so a user must never hand-add, hand-remove, or hand-edit them.
    //
    // Derived / runtime-owned state:
    //   Arcane::WorldTransform    -- recomputed by TransformPropagationSystem
    //                                every frame; an edit would be stomped.
    //   Arcane::PreviousTransform -- the physics-capture interpolation pose.
    //   Arcane::PhysicsBodyRef    -- a live BodyHandle PhysicsSystem owns and
    //                                re-establishes; hand-adding one installs
    //                                a dangling handle.
    // Identity the Outliner owns:
    //   Arcane::EntityInfo        -- documents "the Guid is generated when the
    //                                component is added and is the durable
    //                                cross-save identity". Both real creation
    //                                paths honour that with Guid::Generate(),
    //                                but Edit::AddComponent default-constructs,
    //                                so a generic add stamped the SAME NIL Guid
    //                                on every selected entity and a generic
    //                                remove wiped the name AND that identity for
    //                                the whole selection. The Outliner owns this
    //                                component through create + rename; a
    //                                descriptor-driven add cannot mint a
    //                                per-entity Guid without a post-construct
    //                                hook, which nothing else needs yet.
    //
    // This is THE hide-list. The Inspector's per-component sections, the Add
    // Component catalog, and Remove Component all consult this one predicate,
    // so the three can never drift apart.
    //
    // Everything else is fair game, INCLUDING Arcane::Transform: an entity
    // without one is simply non-spatial (the gizmo and the render systems
    // already skip it), and the removal is undoable like any other.
    [[nodiscard]] bool IsSystemManagedComponent(std::string_view typeName);

    struct ComponentCatalogEntry
    {
        const Astra::ComponentDescriptor* desc = nullptr;
        std::string typeName;           // reflected name, e.g. "Arcane::SpriteRenderer"
        std::size_t missingCount = 0;   // LIVE selected entities lacking it
    };

    // Every reflected, non-system-managed component type registered in `reg`,
    // sorted by type name, filtered by a case-insensitive substring over that
    // name (an empty filter matches everything).
    //
    // `missingCount` counts the LIVE entities in `selection` that do NOT carry
    // the component -- exactly the set Edit::AddComponent would touch. 0 means
    // "every selected entity already has it", so the caller shows the row
    // disabled instead of offering a no-op add. A dead (stale) selection entry
    // is skipped, matching EntityOps' stale-selection tolerance. Duplicate
    // entries in `selection` would double-count; SelectionContext never holds
    // duplicates.
    //
    // UNREFLECTED components are omitted on purpose: the Inspector can only
    // render reflected types, so adding one would be an invisible edit the
    // header menu could never remove.
    [[nodiscard]] std::vector<ComponentCatalogEntry> BuildComponentCatalog(
        Astra::Registry& reg,
        std::span<const Astra::Entity> selection,
        std::string_view filter);
}
