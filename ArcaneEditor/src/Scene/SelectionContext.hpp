#pragma once

// The selected-entity source of truth, shared by outliner, inspector, and
// viewport pick. Ordered multi-select: Entities() keeps selection order
// (front = oldest), Primary() is the last-clicked member -- the entity every
// single-entity consumer (gizmo anchor, inspector, outline) operates on.
// Slice 2 keeps all consumers single-entity via Primary(); slice 3 (full
// multi-edit) fans them out over Entities().
//
// No registry access here: dead entries are swept by Prune() (EditorApp,
// once per frame) and tolerated everywhere else -- EntityOps set ops skip
// dead entities by contract.

#include <Astra/Entity/Entity.hpp>

#include <algorithm>
#include <span>
#include <vector>

namespace Arcane::Editor
{
    struct SelectionContext
    {
        using EntityT = Astra::Entity;

        [[nodiscard]] bool HasSelection() const noexcept { return !m_entities.empty(); }
        [[nodiscard]] std::size_t Count() const noexcept { return m_entities.size(); }
        [[nodiscard]] Astra::Entity Primary() const noexcept { return m_primary; }
        [[nodiscard]] const std::vector<Astra::Entity>& Entities() const noexcept { return m_entities; }
        [[nodiscard]] bool Contains(Astra::Entity e) const noexcept
        {
            return std::find(m_entities.begin(), m_entities.end(), e) != m_entities.end();
        }

        // Plain click: selection becomes exactly { e }.
        void Select(Astra::Entity e)
        {
            m_entities.assign(1, e);
            m_primary = e;
        }

        // Ctrl-click: add (becomes primary) or remove (primary falls back to
        // the most recently selected remaining entry).
        void Toggle(Astra::Entity e)
        {
            auto it = std::find(m_entities.begin(), m_entities.end(), e);
            if (it == m_entities.end())
            {
                m_entities.push_back(e);
                m_primary = e;
            }
            else
            {
                m_entities.erase(it);
                m_primary = m_entities.empty() ? Astra::Entity::Invalid()
                                               : m_entities.back();
            }
        }

        // Shift-range: append `range` in visible-row order, skipping entries
        // already selected; `primary` becomes the primary (the clicked row).
        void AddRange(std::span<const Astra::Entity> range, Astra::Entity primary)
        {
            for (Astra::Entity e : range)
                if (!Contains(e))
                    m_entities.push_back(e);
            if (primary.IsValid())
                m_primary = primary;
        }

        void Clear() noexcept
        {
            m_entities.clear();
            m_primary = Astra::Entity::Invalid();
        }

        // Sweep entries the registry no longer recognizes (after a structural
        // undo/redo swapped the registry object). Primary falls back like
        // Toggle-removal. `alive` is injected so this header stays free of
        // registry includes: sel.Prune([&](Astra::Entity e){ return reg.IsValid(e); });
        template<typename IsAliveFn>
        void Prune(IsAliveFn&& alive)
        {
            std::erase_if(m_entities,
                          [&](Astra::Entity e) { return !alive(e); });
            if (!m_entities.empty() && !Contains(m_primary))
                m_primary = m_entities.back();
            else if (m_entities.empty())
                m_primary = Astra::Entity::Invalid();
        }

    private:
        std::vector<Astra::Entity> m_entities;
        Astra::Entity m_primary = Astra::Entity::Invalid();
    };
}
