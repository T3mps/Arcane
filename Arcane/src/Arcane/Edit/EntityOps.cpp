#include <Arcane/Edit/EntityOps.hpp>

#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneResources.hpp>

#include <Astra/Registry/Registry.hpp>

#include <new>
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
    }

    std::string AutoEntityName(Astra::Registry& reg)
    {
        std::unordered_set<std::string> taken;
        reg.CreateView<EntityInfo>().ForEach(
            [&](Astra::Entity, EntityInfo& info) { taken.insert(info.name); });
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
        if (EntityInfo* info = reg.GetComponent<EntityInfo>(e))
            if (!info->name.empty())
                return info->name;
        return "Entity " + std::to_string(e.GetID());
    }

    Astra::Entity CreateEntity(Astra::Registry& reg, Astra::Entity parent)
    {
        Astra::Entity e = reg.CreateEntity();
        reg.AddComponent<Transform>(e, Transform{});
        reg.AddComponent<EntityInfo>(e, EntityInfo{ Guid::Generate(),
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
        if (EntityInfo* info = reg.GetComponent<EntityInfo>(e))
        {
            if (info->name == name)
                return false;   // no-op rename: no change, no memento
            info->name = std::move(name);
            return true;
        }
        reg.AddComponent<EntityInfo>(e, EntityInfo{ Guid::Generate(),
                                                    std::move(name) });
        return true;
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
