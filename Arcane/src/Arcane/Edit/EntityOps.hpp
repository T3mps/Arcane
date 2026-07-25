#pragma once

// Arcane/Edit: pure structural mutators for editor scene edits (Outliner
// arc). Each function mutates the live registry directly and returns what
// the caller reports; UNDO IS NOT HERE -- the editor wraps every call in a
// RegistryStateCommand (whole-registry memento), which is what keeps these
// simple, reusable, and headless-testable. Engine-side so ArcaneTests and
// every host reuse one implementation; zero editor/UI types.

#include <Arcane/Base/Api.hpp>

#include <Astra/Entity/Entity.hpp>

#include <cstddef>
#include <span>
#include <string>

namespace Astra { class Registry; struct ComponentDescriptor; }

namespace Arcane::Edit
{
    // First of "Entity", "Entity_2", "Entity_3", ... not already used by an
    // EntityInfo in `reg`.
    ARCANE_API std::string AutoEntityName(Astra::Registry& reg);

    // EntityInfo.name when present and non-empty, else "Entity <id>".
    ARCANE_API std::string DisplayName(Astra::Registry& reg, Astra::Entity e);

    // New entity carrying Transform{} + EntityInfo{Generate(), AutoEntityName},
    // parented under `parent` when valid. Returns the new entity.
    ARCANE_API Astra::Entity CreateEntity(Astra::Registry& reg,
                                          Astra::Entity parent);

    // Delete every entity in `set` (duplicates tolerated). Children of a
    // deleted entity first splice up to its nearest NOT-being-deleted
    // ancestor (or to the root when none). Returns entities destroyed.
    ARCANE_API std::size_t DeleteEntities(Astra::Registry& reg,
                                          std::span<const Astra::Entity> set);

    // Reparent every entity in `set` under `parent` (invalid = unparent to
    // root). REFUSES the whole operation (returns 0) when `parent` is inside
    // any moved entity's subtree or is itself in `set` (cycle), or when
    // `parent` is a dead (stale) handle. Skips entities already under
    // `parent`; returns how many moved.
    ARCANE_API std::size_t Reparent(Astra::Registry& reg,
                                    std::span<const Astra::Entity> set,
                                    Astra::Entity parent);

    // Add (hidden=true) or remove the Hidden marker on `e` AND every
    // descendant. Returns how many entities changed state.
    ARCANE_API std::size_t SetHiddenRecursive(Astra::Registry& reg,
                                              Astra::Entity e, bool hidden);

    // Set the display name; adds EntityInfo{Generate(), name} when absent.
    // False only for a dead entity.
    ARCANE_API bool RenameEntity(Astra::Registry& reg, Astra::Entity e,
                                 std::string name);

    // Default-construct `desc`'s component on every entity in `set` that
    // lacks it / remove it from every entity that carries it. Return =
    // entities touched.
    ARCANE_API std::size_t AddComponent(Astra::Registry& reg,
                                        std::span<const Astra::Entity> set,
                                        const Astra::ComponentDescriptor& desc);
    ARCANE_API std::size_t RemoveComponent(Astra::Registry& reg,
                                           std::span<const Astra::Entity> set,
                                           const Astra::ComponentDescriptor& desc);
}
