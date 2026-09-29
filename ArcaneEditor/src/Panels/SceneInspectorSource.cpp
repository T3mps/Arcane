#include "Panels/SceneInspectorSource.hpp"
#include "Panels/SceneSelectionKey.hpp"

#include <Arcane/Edit/EntityOps.hpp>   // Arcane::Edit::DisplayName
#include <Astra/Registry/Registry.hpp>

namespace Arcane::Editor
{
    bool SceneInspectorSource::Alive(Astra::Entity e) const
    {
        return m_deps.registry && e.IsValid() && m_deps.registry->IsValid(e);
    }

    InspectorPage* SceneInspectorSource::Page()
    {
        m_drawSel = m_deps.selection;
        return m_deps.selection ? this : nullptr;
    }

    InspectorPage* SceneInspectorSource::PageFor(std::string_view key)
    {
        using namespace SceneSelectionKey;
        const KeySet alive = AliveSubset(Decode(key), [&](Astra::Entity e) { return Alive(e); });
        if (alive.members.empty()) return nullptr;
        m_pinnedSel.Clear();
        m_pinnedSel.AddRange(alive.members, alive.primary);   // Clear+AddRange keeps order and the named primary; Select would collapse the set
        m_drawSel = &m_pinnedSel;
        return this;
    }

    std::string SceneInspectorSource::SelectionKey() const
    {
        return m_deps.selection ? SceneSelectionKey::Encode(*m_deps.selection) : std::string{};
    }

    bool SceneInspectorSource::Resolves(std::string_view key) const
    {
        using namespace SceneSelectionKey;
        return !AliveSubset(Decode(key), [&](Astra::Entity e) { return Alive(e); }).members.empty();
    }

    bool SceneInspectorSource::RestoreSelection(std::string_view key)
    {
        using namespace SceneSelectionKey;
        if (!m_deps.selection) return false;
        const KeySet alive = AliveSubset(Decode(key), [&](Astra::Entity e) { return Alive(e); });
        if (alive.members.empty()) return false;
        m_deps.selection->Clear();
        m_deps.selection->AddRange(alive.members, alive.primary);
        return true;
    }

    std::vector<InspectorCrumb> SceneInspectorSource::Breadcrumb() const
    {
        std::vector<InspectorCrumb> crumbs;
        crumbs.push_back({ "Scene", [sel = m_deps.selection] { if (sel) sel->Clear(); }, std::nullopt });   // no scene-level page: inert when pinned
        if (m_drawSel && m_drawSel->HasSelection() && m_deps.registry)
        {
            const Astra::Entity primary = m_drawSel->Primary();
            std::string name = Arcane::Edit::DisplayName(*m_deps.registry, primary);
            if (m_drawSel->Count() > 1) name += " (+" + std::to_string(m_drawSel->Count() - 1) + ")";
            crumbs.push_back({ std::move(name),
                               [sel = m_deps.selection, primary] { if (sel) sel->Select(primary); },
                               SceneSelectionKey::Encode(*m_drawSel) });
        }
        return crumbs;
    }

    void SceneInspectorSource::Draw(PropertyGrid&)
    {
        // The scene body draws through InspectorState::grid (Task 1), which
        // IS a PropertyGridState -- the parameter is the page contract's, and
        // the scene page keeps its own persistent state on purpose (gesture
        // token, quat views, search) rather than the instance's.
        if (!m_deps.registry || !m_drawSel || !m_deps.undo || !m_deps.binding || !m_deps.state) return;
        const Arcane::Guid nil;
        DrawInspectorBody(*m_deps.registry, *m_drawSel, *m_deps.undo, *m_deps.binding,
                          m_deps.project, *m_deps.state, m_deps.services,
                          m_deps.selectedAsset ? *m_deps.selectedAsset : nil);
    }
}
