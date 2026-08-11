#include <Arcane/Edit/EntityOps.hpp>

#include <Arcane/Base/Diagnostics.hpp>    // InstantiateSubtrees mirrors LoadJson's skip reporting
#include <Arcane/Base/Log.hpp>            // ARC_WARN
#include <Arcane/Edit/CommandStack.hpp>   // RenameWithUndo brackets its own transaction
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneResources.hpp>
#include <Arcane/Serialization/ReflectionJson.hpp>
#include <Arcane/Serialization/SceneSerializer.hpp>

#include <Astra/Core/TypeID.hpp>
#include <Astra/Registry/Registry.hpp>

#include <new>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Arcane::Edit
{
    namespace
    {
        bool IsAncestorOrSelf(Astra::Registry& reg, Astra::Entity maybeAncestor,
                              Astra::Entity e)
        {
            for (Astra::Entity cur = e; cur.IsValid(); cur = reg.GetParent(cur))
                if (cur == maybeAncestor)
                    return true;
            return false;
        }

        void CollectSubtree(Astra::Registry& reg, Astra::Entity root,
                            std::vector<Astra::Entity>& out)
        {
            out.push_back(root);
            for (Astra::Entity c : reg.GetChildren(root))
                CollectSubtree(reg, c, out);
        }

        // The Identity ComponentDescriptor on `e`, for
        // CommandStack::SnapshotComponent. Registry exposes no
        // descriptor-by-hash accessor, so this walks InspectEntity the way the
        // editor's Inspector loop already does. Matched on the descriptor hash
        // rather than TypeMeta::typeName because that hash IS
        // Astra::TypeID<T>::Hash() by construction, and that hash is a
        // constexpr XXHash64 of the type name (Astra/Core/TypeID.hpp:199-203,
        // :243-246) -- identical in every module, so no string literal has to
        // stay in sync with the type and no cross-DLL id mapping is involved.
        // Null for a dead entity or one that no longer carries Identity.
        const Astra::ComponentDescriptor* FindIdentityDescriptor(Astra::Registry& reg,
                                                                   Astra::Entity e)
        {
            for (const Astra::Registry::ComponentInfo& ci : reg.InspectEntity(e))
                if (ci.descriptor
                    && ci.descriptor->hash == Astra::TypeID<Identity>::Hash())
                    return ci.descriptor;
            return nullptr;
        }
    }

    std::string AutoEntityName(Astra::Registry& reg)
    {
        std::unordered_set<std::string> taken;
        reg.CreateView<Identity>().ForEach(
            [&](Astra::Entity, Identity& info) { taken.insert(info.name); });
        if (!taken.contains("Entity"))
            return "Entity";
        for (int i = 2;; ++i)
        {
            std::string candidate = "Entity_" + std::to_string(i);
            if (!taken.contains(candidate))
                return candidate;
        }
    }

    std::string DisplayName(Astra::Registry& reg, Astra::Entity e)
    {
        if (Identity* info = reg.GetComponent<Identity>(e))
            if (!info->name.empty())
                return info->name;
        return "Entity " + std::to_string(e.GetID());
    }

    Astra::Entity CreateEntity(Astra::Registry& reg, Astra::Entity parent)
    {
        Astra::Entity e = reg.CreateEntity();
        reg.AddComponent<Transform>(e, Transform{});
        reg.AddComponent<Identity>(e, Identity{ Guid::Generate(),
                                                    AutoEntityName(reg) });
        if (parent.IsValid())
            reg.SetParent(e, parent);
        return e;
    }

    Astra::Entity CreateEntityInScene(Astra::Registry& reg, Astra::Entity parent)
    {
        if (parent.IsValid())
            return CreateEntity(reg, parent);
        const SceneRoot* sceneRoot = reg.GetResource<SceneRoot>();
        if (!sceneRoot)
            return Astra::Entity::Invalid();
        return CreateEntity(reg, sceneRoot->entity);
    }

    std::size_t DeleteEntities(Astra::Registry& reg,
                               std::span<const Astra::Entity> set)
    {
        std::unordered_set<Astra::Entity> doomed(set.begin(), set.end());

        // Splice each doomed entity's children to its nearest surviving
        // ancestor BEFORE destroying anything, so subtree structure among
        // survivors is preserved regardless of set nesting.
        for (Astra::Entity e : doomed)
        {
            if (!reg.IsValid(e))
                continue;   // stale selection entry: nothing to splice
            Astra::Entity heir = reg.GetParent(e);
            while (heir.IsValid() && doomed.contains(heir))
                heir = reg.GetParent(heir);
            for (Astra::Entity c : reg.GetChildren(e))
            {
                if (doomed.contains(c))
                    continue;
                if (heir.IsValid())
                    reg.SetParent(c, heir);
                else
                    reg.RemoveParent(c);
            }
        }

        // Dead entities in `set` no-op here rather than count: an honest
        // count keeps ApplyRegistryMutation's count>0 push idiom from
        // pushing a no-op memento (an undo step that reverts nothing).
        std::size_t destroyed = 0;
        for (Astra::Entity e : doomed)
        {
            if (!reg.IsValid(e))
                continue;
            reg.DestroyEntity(e);
            ++destroyed;
        }
        return destroyed;
    }

    std::size_t Reparent(Astra::Registry& reg,
                         std::span<const Astra::Entity> set,
                         Astra::Entity parent)
    {
        // A stale-but-nonnull parent handle passes IsValid() (handle-level
        // non-null) yet SetParent silently no-ops on it below -- refuse the
        // whole operation up front so the caller doesn't count entities as
        // moved when nothing actually changed.
        if (parent.IsValid() && !reg.IsValid(parent))
            return 0;

        if (parent.IsValid())
            for (Astra::Entity e : set)
                if (IsAncestorOrSelf(reg, e, parent))
                    return 0;   // cycle: refuse the whole operation

        std::size_t moved = 0;
        for (Astra::Entity e : set)
        {
            if (!reg.IsValid(e))
                continue;   // stale selection entry: not moved, not counted
            if (reg.GetParent(e) == parent || e == parent)
                continue;
            if (parent.IsValid())
                reg.SetParent(e, parent);
            else
                reg.RemoveParent(e);
            ++moved;
        }
        return moved;
    }

    std::size_t SetHiddenRecursive(Astra::Registry& reg, Astra::Entity e,
                                   bool hidden)
    {
        if (!reg.IsValid(e))
            return 0;   // dead root: descendants come from the live graph,
                        // so a dead root has none to walk
        std::vector<Astra::Entity> subtree;
        CollectSubtree(reg, e, subtree);
        std::size_t changed = 0;
        for (Astra::Entity s : subtree)
        {
            const bool has = reg.GetComponent<Hidden>(s) != nullptr;
            if (hidden && !has)
            {
                reg.AddComponent<Hidden>(s, Hidden{});
                ++changed;
            }
            else if (!hidden && has)
            {
                reg.RemoveComponent<Hidden>(s);
                ++changed;
            }
        }
        return changed;
    }

    bool RenameEntity(Astra::Registry& reg, Astra::Entity e, std::string name)
    {
        if (!reg.IsValid(e))
            return false;
        Identity* info = reg.GetComponent<Identity>(e);
        // Identity is never minted by a rename. UE's equivalents (ActorLabel,
        // ActorGuid) are intrinsic AActor fields (Actor.h:1055/:1188) -- there
        // is no "add identity" edit to mirror. Entities without Identity are
        // runtime spawns with no durable identity to rename.
        if (!info)
            return false;
        if (info->name == name)
            return false;   // no-op rename: no change, no undo step
        info->name = std::move(name);
        return true;
    }

    RenameResult RenameWithUndo(CommandStack& stack, Astra::Registry& reg,
                                Astra::Entity e, const std::string& name)
    {
        // FIRST, and before anything is touched: a rename that JOINED the open
        // transaction would ride its Commit/Cancel, and Cancel discards pending
        // snapshots without reverting (CommandStack.cpp:75-82) -- an applied,
        // permanently un-undoable rename. Refusing costs the caller one retry.
        if (stack.InTransaction())
            return RenameResult::Deferred;

        // These three are what separate "nothing to do" from "cannot be done":
        // RenameEntity itself returns a bare false for all of them plus the
        // unchanged-name case, which a caller cannot act on.
        if (!reg.IsValid(e))
            return RenameResult::Invalid;
        if (!reg.GetComponent<Identity>(e))
            return RenameResult::Invalid;
        const Astra::ComponentDescriptor* desc = FindIdentityDescriptor(reg, e);
        if (!desc)
            return RenameResult::Invalid;   // nothing to snapshot -> no undo coverage

        // The stack is free (checked above), so this scope OWNS its transaction
        // and its dtor commits it. RenameEntity stays the single authority on
        // "did the name actually change": on false, Commit re-snapshots, sees
        // the component unchanged, drops it, and pushes no history entry
        // (CommandStack.cpp:47-51, :61-62).
        ScopedTransaction txn(stack, "Rename");
        txn.Snapshot(e, desc);
        return RenameEntity(reg, e, name) ? RenameResult::Renamed
                                          : RenameResult::NoChange;
    }

    std::size_t AddComponent(Astra::Registry& reg,
                             std::span<const Astra::Entity> set,
                             const Astra::ComponentDescriptor& desc)
    {
        // Mirror of SceneSerializer's add-by-descriptor factory: default-
        // construct into an aligned buffer, add by id, destruct the buffer.
        const std::size_t bytes = desc.size ? desc.size : 1;
        const std::align_val_t align{ desc.alignment ? desc.alignment : 1 };
        std::size_t touched = 0;
        for (Astra::Entity e : set)
        {
            if (reg.HasComponentByHash(e, desc.hash))
                continue;
            void* buf = ::operator new(bytes, align);
            desc.DefaultConstruct(buf);
            if (reg.AddComponentByID(e, desc.id, buf, desc.size))
                ++touched;
            desc.Destruct(buf);
            ::operator delete(buf, align);
        }
        return touched;
    }

    std::size_t RemoveComponent(Astra::Registry& reg,
                                std::span<const Astra::Entity> set,
                                const Astra::ComponentDescriptor& desc)
    {
        std::size_t touched = 0;
        for (Astra::Entity e : set)
            if (reg.RemoveComponentByID(e, desc.id))
                ++touched;
        return touched;
    }

    std::vector<Astra::Entity> SelectionRoots(Astra::Registry& reg,
                                              std::span<const Astra::Entity> set)
    {
        const std::unordered_set<Astra::Entity> members(set.begin(), set.end());
        std::unordered_set<Astra::Entity> emitted;
        std::vector<Astra::Entity> roots;
        for (Astra::Entity e : set)
        {
            if (!reg.IsValid(e) || emitted.contains(e))
                continue;
            // A cycle is impossible through SetParent (it refuses them) but
            // RelationshipGraph::Deserialize installs the parent table
            // unchecked, so a corrupt scene can produce one. Walking it
            // unguarded would hang the editor; stop and treat `e` as a root,
            // which is the safe answer.
            bool covered = false;
            std::unordered_set<Astra::Entity> seen;
            for (Astra::Entity a = reg.GetParent(e); a.IsValid(); a = reg.GetParent(a))
            {
                if (!seen.insert(a).second)
                    break;   // cycle
                if (members.contains(a))
                {
                    covered = true;
                    break;
                }
            }
            if (covered)
                continue;
            emitted.insert(e);
            roots.push_back(e);
        }
        return roots;
    }

    std::vector<Astra::Entity> SubtreeEntities(Astra::Registry& reg,
                                               std::span<const Astra::Entity> roots)
    {
        std::vector<Astra::Entity> out;
        std::unordered_set<Astra::Entity> seen;
        for (Astra::Entity r : roots)
        {
            if (!reg.IsValid(r) || seen.contains(r))
                continue;
            std::vector<Astra::Entity> subtree;
            CollectSubtree(reg, r, subtree);
            for (Astra::Entity e : subtree)
                if (seen.insert(e).second)
                    out.push_back(e);
        }
        return out;
    }

    nlohmann::json SerializeSubtrees(Astra::Registry& reg,
                                     std::span<const Astra::Entity> set)
    {
        nlohmann::json doc;
        doc["version"]  = Scene::kSceneJsonVersion;
        doc["entities"] = nlohmann::json::array();

        const std::vector<Astra::Entity> roots = SelectionRoots(reg, set);
        const std::unordered_set<Astra::Entity> rootSet(roots.begin(), roots.end());
        const std::vector<Astra::Entity> order = SubtreeEntities(reg, roots);

        std::unordered_map<Astra::Entity, int> index;
        for (std::size_t i = 0; i < order.size(); ++i)
            index.emplace(order[i], static_cast<int>(i));
        auto indexOf = [&](Astra::Entity e) -> int
        {
            const auto it = index.find(e);
            return it == index.end() ? -1 : it->second;
        };

        for (Astra::Entity e : order)
        {
            nlohmann::json entry;

            // Per-entity component emit: SaveJson's exact walk
            // (SceneSerializer.hpp) -- reflected roster, null wire shape for
            // zero-size tag components.
            nlohmann::json components = nlohmann::json::object();
            for (const Astra::ComponentDescriptor* desc : reg.GetEntityComponents(e))
            {
                if (!desc || !desc->meta || !desc->visitFields)
                    continue;
                void* instance = const_cast<void*>(
                    static_cast<const Astra::Registry&>(reg).GetComponentByHash(e, desc->hash));
                if (!instance)
                {
                    if (desc->is_empty)
                        components[std::string(desc->meta->typeName)] = nullptr;
                    continue;
                }
                nlohmann::json cj;
                ReflectionJsonWriter writer(cj);
                desc->visitFields(instance, writer);
                components[std::string(desc->meta->typeName)] = std::move(cj);
            }
            entry["components"] = std::move(components);

            entry["parent"] = indexOf(reg.GetParent(e));
            if (rootSet.contains(e))
            {
                const Astra::Entity parent = reg.GetParent(e);
                if (parent.IsValid() && reg.IsValid(parent))
                    if (const Identity* info = reg.GetComponent<Identity>(parent))
                        if (info->id.IsValid())
                            entry["rootParentGuid"] = info->id.ToString();
            }

            // Internal links only, forward edges (SaveJson's rule).
            const int self = indexOf(e);
            nlohmann::json links = nlohmann::json::array();
            for (Astra::Entity linked : reg.GetRelationshipGraph().GetLinks(e))
            {
                const int j = indexOf(linked);
                if (j > self)
                    links.push_back(j);
            }
            if (!links.empty())
                entry["links"] = std::move(links);

            doc["entities"].push_back(std::move(entry));
        }
        return doc;
    }

    std::vector<Astra::Entity> InstantiateSubtrees(Astra::Registry& reg,
                                                   const nlohmann::json& payload)
    {
        const SceneRoot* sceneRoot = reg.GetResource<SceneRoot>();
        if (!sceneRoot)
            return {};   // nowhere safe to live -- CreateEntityInScene's rule

        std::vector<Astra::Entity> created;
        const auto destroyPartial = [&]() -> std::vector<Astra::Entity>
        {
            for (Astra::Entity e : created)
                if (reg.IsValid(e))
                    reg.DestroyEntity(e);
            return {};
        };

        try
        {
            if (!payload.is_object() || !payload.contains("entities"))
                return {};
            const auto vit = payload.find("version");
            if (vit == payload.end() || !vit->is_number_integer() ||
                vit->get<int>() != Scene::kSceneJsonVersion)
                return {};
            const auto& entities = payload["entities"];
            if (!entities.is_array() || entities.empty())
                return {};

            // Pre-existing state, captured BEFORE creating anything: the guid
            // map for rootParentGuid targets (fresh guids can never match),
            // and the taken-name set for uniquify.
            std::unordered_map<std::string, Astra::Entity> byGuid;
            std::unordered_set<std::string> taken;
            reg.CreateView<Identity>().ForEach(
                [&](Astra::Entity e, Identity& info)
                {
                    if (info.id.IsValid())
                        byGuid.emplace(info.id.ToString(), e);
                    taken.insert(info.name);
                });

            Astra::ComponentRegistry* creg = reg.GetComponentRegistry();

            // Accumulated across Pass 1 and published ONCE right after it, same
            // placement as LoadJson's own publish (SceneSerializer.hpp:339):
            // a skip mid-walk that later triggers destroyPartial() must not
            // leave stale rows pointing at destroyed entities, so this is
            // never published on an early return, only on Pass 1 completing.
            std::vector<Arcane::Diagnostic> diagnostics;
            std::size_t entityIndex = 0;

            // Pass 1: create + components (LoadJson's tolerant per-entry walk).
            for (const auto& entry : entities)
            {
                if (!entry.is_object())
                    return destroyPartial();
                const Astra::Entity e = reg.CreateEntity();
                created.push_back(e);

                const auto cit = entry.find("components");
                if (cit == entry.end() || !cit->is_object())
                {
                    ++entityIndex;
                    continue;
                }
                static const nlohmann::json kEmptyFields = nlohmann::json::object();
                for (auto it = cit->begin(); it != cit->end(); ++it)
                {
                    const nlohmann::json* fields;
                    if (it.value().is_object())    fields = &it.value();
                    else if (it.value().is_null()) fields = &kEmptyFields;
                    else                            continue;
                    const Scene::Detail::AddComponentResult r =
                        Scene::Detail::AddComponentByTypeName(reg, creg, e, it.key(), *fields);
                    if (r == Scene::Detail::AddComponentResult::Error)
                        return destroyPartial();   // unsupported field type: fail loud

                    // A paste is MORE likely than a scene load to meet a type
                    // the destination process doesn't know: the payload was
                    // authored under whatever plugin roster the SOURCE
                    // registry had loaded, which the paste target need not
                    // share. Silently dropping it here would be the exact
                    // "no trace anywhere" loss LoadJson's own WARN was added
                    // to fix (SceneSerializer.hpp:145-151) -- mirror it.
                    if (r == Scene::Detail::AddComponentResult::SkippedUnknownType)
                    {
                        ARC_WARN("paste: unknown component \"{}\" skipped on "
                                 "entity #{} (id {}, v{}) -- pasted without it",
                                 it.key(), entityIndex, e.GetID(),
                                 static_cast<unsigned>(e.GetVersion()));
                        Arcane::Diagnostic d;
                        d.severity = Arcane::DiagSeverity::Error;
                        d.scope    = Arcane::DiagScope::Scene;
                        d.code     = "clipboard.component.unknown";
                        d.message  = "Unknown component \"" + std::string(it.key()) +
                                     "\" on pasted entity #" + std::to_string(entityIndex);
                        d.detail   = "Pasted without this component.";
                        d.locator  = Arcane::DiagLocator::Entity(
                                         static_cast<std::uint64_t>(e.GetValue()));
                        diagnostics.push_back(std::move(d));
                    }
                    else if (r == Scene::Detail::AddComponentResult::SkippedUnregistered)
                    {
                        ARC_WARN("paste: component \"{}\" is reflected but not "
                                 "registered (plugin not loaded?) -- skipped on "
                                 "entity #{} (id {}, v{}); pasted without it",
                                 it.key(), entityIndex, e.GetID(),
                                 static_cast<unsigned>(e.GetVersion()));
                        Arcane::Diagnostic d;
                        d.severity = Arcane::DiagSeverity::Error;
                        d.scope    = Arcane::DiagScope::Scene;
                        d.code     = "clipboard.component.unregistered";
                        d.message  = "Component \"" + std::string(it.key()) +
                                     "\" is reflected but not registered (plugin not "
                                     "loaded?) on pasted entity #" + std::to_string(entityIndex);
                        d.detail   = "Pasted without this component.";
                        d.locator  = Arcane::DiagLocator::Entity(
                                         static_cast<std::uint64_t>(e.GetValue()));
                        diagnostics.push_back(std::move(d));
                    }
                }
                ++entityIndex;
            }

            // Unconditional, like LoadJson's own publish: an empty vector
            // retracts a previous paste's rows under this key (the clean-
            // paste case).
            Arcane::Diagnostics::Publish("clipboard", diagnostics);

            // Pass 2: fresh identity -- new Guid, uniquified name (paste/
            // duplicate unique-ify; the taken set grows so N pastes of "Foo"
            // yield Foo_2, Foo_3, ...).
            for (Astra::Entity e : created)
            {
                Identity* info = reg.GetComponent<Identity>(e);
                if (!info)
                    continue;
                info->id = Guid::Generate();
                std::string name = info->name.empty() ? std::string("Entity") : info->name;
                if (taken.contains(name))
                {
                    for (int i = 2;; ++i)
                    {
                        std::string candidate = name + "_" + std::to_string(i);
                        if (!taken.contains(candidate))
                        {
                            name = std::move(candidate);
                            break;
                        }
                    }
                }
                taken.insert(name);
                info->name = std::move(name);
            }

            // Pass 3: structure. Internal parents/links remap; roots go to
            // their recorded parent when it still lives, else SceneRoot.
            std::vector<Astra::Entity> roots;
            for (std::size_t i = 0; i < entities.size(); ++i)
            {
                const auto& entry = entities[i];
                int parent = -1;
                if (const auto pit = entry.find("parent");
                    pit != entry.end() && pit->is_number_integer())
                    parent = pit->get<int>();

                if (parent >= 0 && parent < static_cast<int>(created.size()))
                {
                    reg.SetParent(created[i], created[static_cast<std::size_t>(parent)]);
                }
                else
                {
                    Astra::Entity target = sceneRoot->entity;
                    if (const auto git = entry.find("rootParentGuid");
                        git != entry.end() && git->is_string())
                    {
                        const auto hit = byGuid.find(git->get<std::string>());
                        if (hit != byGuid.end() && reg.IsValid(hit->second))
                            target = hit->second;
                    }
                    reg.SetParent(created[i], target);
                    roots.push_back(created[i]);
                }

                if (const auto lit = entry.find("links");
                    lit != entry.end() && lit->is_array())
                {
                    for (const auto& lj : *lit)
                    {
                        if (!lj.is_number_integer())
                            continue;
                        const int j = lj.get<int>();
                        if (j >= 0 && j < static_cast<int>(created.size()))
                            reg.AddLink(created[i], created[static_cast<std::size_t>(j)]);
                    }
                }
            }
            return roots;
        }
        catch (...)
        {
            return destroyPartial();   // exception-free contract at the API edge
        }
    }

    glm::mat3 WorldMatrix(Astra::Registry& reg, Astra::Entity e)
    {
        // Walk up to the root collecting the chain, then fold the local
        // matrices back down root-first. Cycle-safe like SelectionRoots: a
        // corrupt deserialized parent table can cycle, so a visited set stops
        // the climb instead of spinning.
        std::vector<Astra::Entity> chain;
        std::unordered_set<Astra::Entity> seen;
        for (Astra::Entity cur = e; cur.IsValid(); cur = reg.GetParent(cur))
        {
            if (!seen.insert(cur).second)
                break;   // cycle: stop climbing, fold what was collected
            chain.push_back(cur);
        }

        glm::mat3 m(1.0f);
        for (auto it = chain.rbegin(); it != chain.rend(); ++it)
        {
            if (Transform* t = reg.GetComponent<Transform>(*it))
                m = m * t->ToMatrix();
        }
        return m;
    }

    glm::mat3 ParentWorldMatrix(Astra::Registry& reg, Astra::Entity e)
    {
        Astra::Entity parent = reg.GetParent(e);
        if (!parent.IsValid())
            return glm::mat3(1.0f);
        return WorldMatrix(reg, parent);   // cycle-safe via WorldMatrix's own visited set
    }
}
