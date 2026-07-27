#pragma once

// Arcane/Edit: pure structural mutators for editor scene edits (Outliner
// arc). Each function mutates the live registry directly and returns what
// the caller reports; UNDO IS NOT HERE -- the editor wraps every call in a
// RegistryStateCommand (whole-registry memento), which is what keeps these
// simple, reusable, and headless-testable. Engine-side so ArcaneTests and
// every host reuse one implementation; zero editor/UI types.

#include <Arcane/Base/Api.hpp>

#include <Astra/Entity/Entity.hpp>

#include <glm/glm.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <vector>

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
    // A dead (stale) parent handle silently falls back to root creation --
    // Registry::SetParent no-ops on dead parents and a create is still a
    // real edit worth keeping.
    ARCANE_API Astra::Entity CreateEntity(Astra::Registry& reg,
                                          Astra::Entity parent);

    // Like CreateEntity, but for editor call sites that mean "add to the
    // scene" rather than "add anywhere in the registry". With a valid
    // `parent` this is exactly CreateEntity (the Outliner row's "New Child
    // Entity" contract is unchanged). With an invalid `parent` it attaches
    // under the SceneRoot entity instead of leaving the new entity an
    // unparented sibling of SceneRoot -- SceneSerializer::SaveJson and
    // TransformPropagationSystem both walk ONLY the SceneRoot subtree, so a
    // sibling of it never renders and is silently dropped by the next Save.
    // Refuses (returns Astra::Entity::Invalid(), creates nothing) when there
    // is no SceneRoot resource at all: an entity created here would have
    // nowhere safe to live, and DoSaveScene already refuses to save a
    // rootless registry (EditorApp.cpp), so creating one would only relocate
    // the same data-loss bug rather than fix it. A refused create is
    // recoverable (open or start a scene, then try again); a silent create-
    // then-lose is not.
    ARCANE_API Astra::Entity CreateEntityInScene(Astra::Registry& reg,
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
    // Returns false for a dead entity OR when the name is already exactly
    // `name` (no-op).
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

    // The entities in `set` that have NO ancestor also in `set`. Transform
    // edits must apply to these only: moving a parent already carries its
    // children through WorldTransform propagation, so applying to both
    // double-moves the children. Dead entities are skipped and duplicates
    // collapse; surviving order follows `set`.
    ARCANE_API std::vector<Astra::Entity> SelectionRoots(Astra::Registry& reg,
                                                         std::span<const Astra::Entity> set);

    // World matrix of `e`: the product of Transform::ToMatrix up the parent
    // chain (identity for a missing Transform at any level). Computed from the
    // live graph rather than the WorldTransform component, which is only
    // refreshed for SceneRoot's subtree.
    ARCANE_API glm::mat3 WorldMatrix(Astra::Registry& reg, Astra::Entity e);

    // World matrix of `e`'s PARENT (identity when it has none) -- invert this
    // to convert a world pose back into `e`'s local Transform.
    ARCANE_API glm::mat3 ParentWorldMatrix(Astra::Registry& reg, Astra::Entity e);
}
