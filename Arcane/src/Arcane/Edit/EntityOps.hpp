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

#include <Json.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace Astra { class Registry; struct ComponentDescriptor; }

namespace Arcane { class CommandStack; }

namespace Arcane::Edit
{
    // First of "Entity", "Entity_2", "Entity_3", ... not already used by an
    // Identity in `reg`.
    ARCANE_API std::string AutoEntityName(Astra::Registry& reg);

    // Identity.name when present and non-empty, else "Entity <id>".
    ARCANE_API std::string DisplayName(Astra::Registry& reg, Astra::Entity e);

    // New entity carrying Transform{} + Identity{Generate(), AutoEntityName},
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

    // Rename an existing Identity. Returns false (mutating nothing) when the
    // entity is invalid, lacks Identity, or the name is unchanged -- so a
    // false return always means "no undo step needed". Never adds the
    // component: identity is minted at creation only.
    //
    // Names are deliberately NOT unique. Uniqueness is a creation-time policy
    // (AutoEntityName); duplicates are legal on rename -- UE's split exactly.
    // Spawn/paste/duplicate go through SetActorLabelUnique
    // (EditorEngine.cpp:6154), while the outliner's rename commit calls
    // FActorLabelUtilities::RenameExistingActor with TWO arguments
    // (ActorTreeItem.cpp:269) and its third parameter `bMakeUnique` defaults to
    // FALSE (EditorEngine.h:3365) -- so an interactive rename in UE takes the
    // exact label typed, duplicates included.
    ARCANE_API bool RenameEntity(Astra::Registry& reg, Astra::Entity e,
                                 std::string name);

    // What RenameWithUndo did. Only `Renamed` produced a history entry.
    enum class RenameResult
    {
        Renamed,    // name changed; exactly one "Rename" transaction pushed
        NoChange,   // name already equal -- nothing mutated, nothing pushed
        Invalid,    // dead entity, no Identity, or no findable descriptor
        Deferred,   // a transaction was already open; NOTHING was mutated
    };

    // Rename bracketed in its OWN undo transaction. Refuses (returns Deferred)
    // while another transaction is open rather than joining it: a joined rename
    // rides the owner's Commit/Cancel, and Cancel discards pending snapshots
    // WITHOUT reverting (CommandStack.cpp:75-82) -- the rename would apply but
    // be permanently un-undoable. The caller re-tries next frame.
    //
    // Deferred is checked FIRST and mutates nothing, so a caller that parks the
    // request and retries loses no edit. The descriptor for Identity is found
    // here (Registry exposes no descriptor-by-hash accessor, so this walks
    // InspectEntity and matches on ci.descriptor->hash, which IS
    // Astra::TypeID<Identity>::Hash() by construction) -- keeping that lookup
    // engine-side is what lets every host share one implementation instead of
    // re-deriving the undo shape per panel.
    ARCANE_API RenameResult RenameWithUndo(CommandStack& stack, Astra::Registry& reg,
                                           Astra::Entity e, const std::string& name);

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

    // Every entity of every subtree rooted in `roots` (each root + all its
    // descendants; duplicates collapse, dead roots skip). The set a Cut must
    // hand DeleteEntities: DeleteEntities SPLICES children up to survivors,
    // so cutting only the roots would orphan the children the clipboard just
    // captured.
    ARCANE_API std::vector<Astra::Entity> SubtreeEntities(Astra::Registry& reg,
                                                          std::span<const Astra::Entity> roots);

    // Serialize the subtrees rooted at SelectionRoots(set) -- a nested
    // selection copies once. Same {"version","entities"} document SaveJson
    // produces (components via the reflection walk, "parent" = payload-
    // internal index, -1 for roots; internal "links" kept), plus each ROOT
    // entry records "rootParentGuid" -- the Identity Guid of its ORIGINAL
    // parent (key absent when the parent has no Identity or none exists).
    // "entities" is empty when `set` holds nothing alive.
    ARCANE_API nlohmann::json SerializeSubtrees(Astra::Registry& reg,
                                                std::span<const Astra::Entity> set);

    // Instantiate a SerializeSubtrees payload: fresh entities with fresh
    // Identity GUIDs and names uniquified against the registry (paste/
    // duplicate unique-ify, interactive rename does not -- the UE split
    // documented on RenameEntity above). Internal parents/links remap to the
    // new entities; each root re-parents to the live entity whose Identity
    // Guid matches its recorded rootParentGuid, else under SceneRoot --
    // one rule that makes Duplicate a sibling and cross-instance Paste sane.
    // Returns the created ROOT entities. Refuses (returns {}, creates
    // nothing lasting) when the registry has no SceneRoot, on a version
    // mismatch, on a malformed document, or on a component field error --
    // partial creations are destroyed (all-or-nothing). A component type the
    // destination registry cannot instantiate (unknown or reflected-but-
    // unregistered -- likely on a cross-plugin-roster paste) does NOT refuse
    // the paste; it is skipped and reported the way LoadJson reports the same
    // two cases (SceneSerializer.hpp) -- ARC_WARN plus an Arcane::Diagnostic
    // published under the "clipboard" key.
    ARCANE_API std::vector<Astra::Entity> InstantiateSubtrees(Astra::Registry& reg,
                                                              const nlohmann::json& payload);

    // World matrix of `e`: the product of Transform::ToMatrix up the parent
    // chain (identity for a missing Transform at any level). Computed from the
    // live graph rather than the WorldTransform component, which is only
    // refreshed for SceneRoot's subtree.
    ARCANE_API glm::mat3 WorldMatrix(Astra::Registry& reg, Astra::Entity e);

    // World matrix of `e`'s PARENT (identity when it has none) -- invert this
    // to convert a world pose back into `e`'s local Transform.
    ARCANE_API glm::mat3 ParentWorldMatrix(Astra::Registry& reg, Astra::Entity e);
}
