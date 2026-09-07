#include <Panels/AssetPanelModel.hpp>

#include <Arcane/Project/AssetRegistry.hpp>

#include <algorithm>
#include <map>
#include <set>

namespace Arcane::Editor
{
    namespace
    {
        // folder = the directory portion of the mount path after "scheme://";
        // root files fold into the synthetic "Content/" bucket (spec s5/s6).
        // name = stem, fileName = stem + extension (rows show this).
        AssetPanelEntry MakeBaseEntry(const Arcane::Guid& guid, const std::string& mountPath)
        {
            AssetPanelEntry e;
            e.guid = guid;
            e.mountPath = mountPath;
            e.kind = AssetKindOf(mountPath);

            std::string_view rest = mountPath;
            if (const std::size_t scheme = rest.find("://"); scheme != std::string_view::npos)
                rest = rest.substr(scheme + 3);

            const std::size_t slash = rest.rfind('/');
            const std::string_view file = (slash == std::string_view::npos)
                                               ? rest : rest.substr(slash + 1);
            e.folder = (slash == std::string_view::npos)
                           ? std::string("Content/")
                           : std::string(rest.substr(0, slash + 1));

            e.fileName = std::string(file);
            const std::size_t dot = file.rfind('.');
            e.name = std::string(dot == std::string_view::npos ? file : file.substr(0, dot));
            return e;
        }

        // Bridges an AssetPanelEntry into the AssetEntry shape so
        // MatchesFilter's case-insensitive name/mount-path search is reused
        // verbatim rather than re-implemented here.
        AssetEntry ToAssetEntry(const AssetPanelEntry& e)
        {
            AssetEntry a;
            a.guid = e.guid;
            a.mountPath = e.mountPath;
            a.name = e.name;
            a.kind = e.kind;
            return a;
        }
    }

    CookState CookStateOf(AssetKind kind, bool permanentDiag, bool pending)
    {
        // A permanent refusal always wins -- it says nothing further will
        // ever happen for this guid without a user fixing the source, so it
        // outranks any pending-cook signal regardless of kind.
        if (permanentDiag)
            return CookState::Refused;

        // Only Texture/Sprite have a real cook pipeline of their own (see
        // this function's own header comment) -- everything else (materials,
        // scenes, meshes, data, ...) has nothing to be "pending" about, so
        // IsCookPending's own default-true answer for a guid with no row
        // must never leak through as a permanent Queued state for them.
        const bool cooks = (kind == AssetKind::Texture) || (kind == AssetKind::Sprite);
        if (!cooks)
            return CookState::Cooked;

        return pending ? CookState::Queued : CookState::Cooked;
    }

    void AssetPanelModel::MarkDirty(const Arcane::Guid& id)
    {
        if (!m_allDirty)
            m_dirty.insert(id);
    }

    void AssetPanelModel::MarkAllDirty()
    {
        m_allDirty = true;
        m_dirty.clear();
    }

    bool AssetPanelModel::RebuildIfDirty(const Arcane::AssetRegistry* registry,
                                         const AssetPanelProviders& p)
    {
        if (!registry)
        {
            const bool hadAnything = !m_entries.empty() || !m_rows.empty() || !m_rail.empty();
            m_entries.clear();
            m_dirty.clear();
            m_allDirty = false;
            m_rows.clear();
            m_rail.clear();
            m_shownAssetCount = 0;
            m_rowsDirty = false;
            return hadAnything;
        }

        if (!m_allDirty && m_dirty.empty() && !m_rowsDirty)
            return false;   // cheap path: nothing to do

        const std::vector<std::pair<Arcane::Guid, std::string>> all = registry->All();

        // Live guid -> mount path: target-kind lookups for fold/sliced below,
        // and to drop cache entries the registry no longer carries.
        std::unordered_map<Arcane::Guid, const std::string*> live;
        live.reserve(all.size());
        for (const auto& [guid, mountPath] : all)
            live.emplace(guid, &mountPath);

        bool entriesChanged = false;

        // Anything folded under a guid the registry no longer carries must be
        // re-evaluated THIS pass, even if only the removed guid itself was
        // dirtied (fix round 1): a dependent's cached `foldedUnder` is
        // otherwise left pointing at a now-gone guid -- still IsValid() (it
        // only checks non-nil), so the dependent is silently excluded from
        // byFolder (not a peer, foldedUnder still "valid") AND never re-added
        // to any parent's derivedChildren (the parent entry is gone) -- it
        // vanishes from Rows() entirely. Cascading into m_dirty re-asks the
        // providers only for these directly-affected dependents (one level:
        // a folded child can never itself be a fold parent), never for the
        // rest of the registry -- the per-guid MarkDirty guarantee (test f)
        // stays intact.
        std::unordered_set<Arcane::Guid> cascadeDirty;
        for (auto it = m_entries.begin(); it != m_entries.end(); )
        {
            if (!live.count(it->first))
            {
                for (const Arcane::Guid& child : it->second.derivedChildren)
                    cascadeDirty.insert(child);
                it = m_entries.erase(it);
                entriesChanged = true;
            }
            else ++it;
        }
        if (!m_allDirty)
            for (const Arcane::Guid& guid : cascadeDirty)
                m_dirty.insert(guid);

        auto rebuildOne = [&](const Arcane::Guid& guid, const std::string& mountPath)
        {
            AssetPanelEntry e = MakeBaseEntry(guid, mountPath);

            if (e.kind == AssetKind::Material && p.surfaceFor)
                e.surface = p.surfaceFor(guid);

            const std::optional<std::vector<Arcane::AssetRef>> refs =
                p.refsFor ? p.refsFor(guid) : std::nullopt;

            int derivesFromCount = 0;
            Arcane::Guid derivesFromTarget;
            bool hasTextureReference = false;   // a References-kind ref to a texture
            if (refs)
            {
                for (const Arcane::AssetRef& ref : *refs)
                {
                    if (ref.kind == Arcane::AssetRefKind::DerivesFrom)
                    {
                        ++derivesFromCount;
                        derivesFromTarget = ref.target;
                    }
                    else if (ref.kind == Arcane::AssetRefKind::References)
                    {
                        const auto itT = live.find(ref.target);
                        if (itT != live.end() && AssetKindOf(*itT->second) == AssetKind::Texture)
                            hasTextureReference = true;
                    }
                }
            }

            // An instance material carries a `parent` DerivesFrom -- see
            // ListAssetReferences's own doc comment (Assets.hpp). Only
            // meaningful for materials: a folding sprite's DerivesFrom names a
            // TEXTURE, never another material.
            e.isInstance = (e.kind == AssetKind::Material) && (derivesFromCount > 0);

            // Fold: exactly one DerivesFrom, and it resolves to a texture.
            if (derivesFromCount == 1)
            {
                const auto itT = live.find(derivesFromTarget);
                if (itT != live.end() && AssetKindOf(*itT->second) == AssetKind::Texture)
                    e.foldedUnder = derivesFromTarget;
            }

            // Sliced: a sprite that did NOT fold but still names a texture via
            // a References-kind ref (controller ruling, Task 4).
            e.sliced = (e.kind == AssetKind::Sprite) && !e.foldedUnder.IsValid() && hasTextureReference;

            e.cook = p.cookStateFor ? p.cookStateFor(guid) : CookState::Unknown;

            m_entries[guid] = std::move(e);
        };

        if (m_allDirty)
        {
            for (const auto& [guid, mountPath] : all)
                rebuildOne(guid, mountPath);
            entriesChanged = true;
        }
        else if (!m_dirty.empty())
        {
            for (const Arcane::Guid& guid : m_dirty)
            {
                const auto it = live.find(guid);
                if (it != live.end())
                    rebuildOne(guid, *it->second);
            }
            entriesChanged = true;
        }

        if (entriesChanged)
        {
            // Aggregate derivedChildren from foldedUnder -- a pure re-
            // derivation over ALREADY-CACHED entries, no provider calls. This
            // is what keeps a single-guid MarkDirty cheap: only the dirtied
            // guid's provider callables were invoked above, never its peers'.
            for (auto& [guid, e] : m_entries)
                e.derivedChildren.clear();
            for (auto& [guid, e] : m_entries)
            {
                if (!e.foldedUnder.IsValid())
                    continue;
                const auto pit = m_entries.find(e.foldedUnder);
                if (pit != m_entries.end())
                    pit->second.derivedChildren.push_back(guid);
            }
            for (auto& [guid, e] : m_entries)
            {
                std::sort(e.derivedChildren.begin(), e.derivedChildren.end(),
                         [this](const Arcane::Guid& a, const Arcane::Guid& b)
                         { return m_entries.at(a).fileName < m_entries.at(b).fileName; });
            }
        }

        m_dirty.clear();
        m_allDirty = false;

        if (entriesChanged)
            m_rowsDirty = true;

        if (m_rowsDirty)
        {
            RebuildRows();
            m_rowsDirty = false;
        }

        return true;
    }

    void AssetPanelModel::SetSearch(std::string_view s)
    {
        if (m_search == s)
            return;
        m_search = std::string(s);
        m_rowsDirty = true;
    }

    void AssetPanelModel::SetKindFilter(int kindOrMinus1)
    {
        if (m_kindFilter == kindOrMinus1)
            return;
        m_kindFilter = kindOrMinus1;
        m_rowsDirty = true;
    }

    void AssetPanelModel::SetGroupOpen(const std::string& folder, bool open)
    {
        m_groupOpen[folder] = open;
        m_rowsDirty = true;
    }

    void AssetPanelModel::SetChildrenOpen(const Arcane::Guid& texture, bool open)
    {
        m_childrenOpen[texture] = open;
        m_rowsDirty = true;
    }

    bool AssetPanelModel::MatchesEntryFilter(const AssetPanelEntry& e) const
    {
        return MatchesFilter(ToAssetEntry(e), m_kindFilter, m_search);
    }

    namespace
    {
        // m_groupOpen's own default (OPEN) -- shared by the ancestor-chain walk
        // and each folder's own toggle below, so both read the identical rule.
        bool GroupOpenOrDefault(const std::unordered_map<std::string, bool>& groupOpen,
                                const std::string& folder)
        {
            const auto it = groupOpen.find(folder);
            return it == groupOpen.end() ? true : it->second;
        }

        // True iff EVERY strict ancestor of `folder` (its parent, grandparent, ...
        // up to the top-level dir/"Content/" root) is open. `folder` itself is
        // deliberately excluded -- a group's OWN closed flag gates its OWN
        // children (RebuildRows' showChildren), never whether its own row
        // renders (spec s6: "collapsing a parent hides its whole subtree", but
        // the collapsed parent's own header stays visible -- today's flat
        // behavior, unchanged by nesting). A folder with no group row of its
        // own (no direct entries -- never toggled in the UI) simply defaults
        // open here, which is the correct no-op.
        bool AncestorsOpen(const std::unordered_map<std::string, bool>& groupOpen,
                           std::string_view folder)
        {
            std::string parent = GroupParentOf(folder);
            while (!parent.empty())
            {
                if (!GroupOpenOrDefault(groupOpen, parent))
                    return false;
                parent = GroupParentOf(parent);
            }
            return true;
        }
    }

    void AssetPanelModel::RebuildRows()
    {
        m_rows.clear();
        m_shownAssetCount = 0;

        // Rail counts ignore the kind filter (the rail is what PICKS it) but
        // honor search, and count folded children too -- the same "folded
        // children included" rule Health().total states, extended to this
        // per-kind view.
        int kindCounts[kAssetKindCount] = {};

        // Per-folder, TOP-LEVEL (unfolded) entries that pass BOTH filters --
        // folded children are never folder peers; they render nested under
        // their parent instead (fold semantics). Keyed by the FULL content-
        // directory path -- std::map's lexicographic order over strings that
        // all carry a trailing '/' is already a valid tree PREORDER (a
        // directory's own key is always immediately followed by every one of
        // its descendants, before any later sibling subtree: '/' (0x2F) sorts
        // below every letter/digit a real path segment starts with, so
        // "textures/" < "textures/patterns/" < "textures0/" holds generally).
        // This is what lets the single sorted-iteration loop below double as
        // the nested-group walk with no separate tree structure.
        std::map<std::string, std::vector<const AssetPanelEntry*>> byFolder;

        for (const auto& [guid, e] : m_entries)
        {
            if (MatchesFilter(ToAssetEntry(e), -1, m_search))
                ++kindCounts[static_cast<int>(e.kind)];

            const bool matches = MatchesEntryFilter(e);
            if (matches)
                ++m_shownAssetCount;

            if (!e.foldedUnder.IsValid() && matches)
                byFolder[e.folder].push_back(&e);
        }

        for (auto& [folder, vec] : byFolder)
        {
            std::sort(vec.begin(), vec.end(),
                     [](const AssetPanelEntry* a, const AssetPanelEntry* b)
                     { return a->fileName < b->fileName; });
        }

        // Which folders get a GROUP ROW at all. Base rule (unchanged): only
        // folders with >=1 own matching entry ("groups with zero visible rows
        // are... dropped", spec s6). Search addendum (2026-09-07): while
        // searching, a folder with ZERO own matches still gets a (groupCount==0)
        // context row if it is an ANCESTOR of some folder that does -- "a
        // matching row under a collapsed ancestor still shows, with its
        // ancestor group rows shown". Outside search this bridging row is never
        // synthesized: an intermediate directory with no files of its own and
        // only nested subfolders does not currently render its own group row
        // (no fixture in this arc has that shape -- see the impl report).
        const bool searchActive = !m_search.empty();
        std::set<std::string> renderFolders;
        for (const auto& [folder, vec] : byFolder)
            renderFolders.insert(folder);
        if (searchActive)
        {
            for (const auto& [folder, vec] : byFolder)
            {
                std::string parent = GroupParentOf(folder);
                while (!parent.empty())
                {
                    renderFolders.insert(parent);
                    parent = GroupParentOf(parent);
                }
            }
        }

        // std::set<std::string> iterates sorted -- the same valid-preorder
        // property byFolder relies on above.
        for (const std::string& folder : renderFolders)
        {
            const int depth = GroupDepthOf(folder);
            const auto vecIt = byFolder.find(folder);
            const int ownCount = (vecIt != byFolder.end()) ? static_cast<int>(vecIt->second.size()) : 0;

            // Cascading collapse: an ancestor's closed flag hides this row (and
            // therefore everything under it, transitively, via each descendant's
            // own AncestorsOpen check) UNLESS search is active, in which case
            // every renderFolders entry is, by construction, on the path to a
            // real match and shows unconditionally (2026-09-07 ruling: search
            // overrides collapse at every level of the chain, including a row's
            // own immediate group -- the direct generalization of the existing
            // fold-child "one level" rule to an arbitrary-depth chain; see the
            // impl report for why this is a deliberate reading, not a literal
            // one-line spec quote).
            const bool visible = searchActive || AncestorsOpen(m_groupOpen, folder);
            if (!visible)
                continue;

            AssetPanelRow group;
            group.type = AssetPanelRow::Type::Group;
            group.groupName = folder;                    // full path -- the open-state key
            group.groupLabel = GroupLabelOf(folder);      // leaf segment only
            group.groupDepth = depth;
            group.groupCount = ownCount;
            m_rows.push_back(std::move(group));

            // This folder's OWN closed flag gates its OWN content (its direct
            // asset rows below, and -- transitively, via the next iterations'
            // own AncestorsOpen check -- any nested subgroup) -- same override
            // under search as the ancestor check just above.
            const bool showChildren = searchActive || GroupOpenOrDefault(m_groupOpen, folder);
            if (!showChildren || vecIt == byFolder.end())
                continue;

            for (const AssetPanelEntry* parent : vecIt->second)
            {
                AssetPanelRow row;
                row.type = AssetPanelRow::Type::Asset;
                row.guid = parent->guid;
                row.groupDepth = depth;
                m_rows.push_back(row);

                if (parent->derivedChildren.empty())
                    continue;

                const auto childIt = m_childrenOpen.find(parent->guid);
                const bool childrenOpen = (childIt != m_childrenOpen.end()) && childIt->second;   // default COLLAPSED
                if (!childrenOpen)
                    continue;

                std::vector<const AssetPanelEntry*> children;
                children.reserve(parent->derivedChildren.size());
                for (const Arcane::Guid& childGuid : parent->derivedChildren)
                {
                    const auto cit = m_entries.find(childGuid);
                    if (cit == m_entries.end())
                        continue;
                    if (!MatchesEntryFilter(cit->second))   // children match search INDEPENDENTLY
                        continue;
                    children.push_back(&cit->second);
                }
                std::sort(children.begin(), children.end(),
                         [](const AssetPanelEntry* a, const AssetPanelEntry* b)
                         { return a->fileName < b->fileName; });

                for (const AssetPanelEntry* child : children)
                {
                    AssetPanelRow childRow;
                    childRow.type = AssetPanelRow::Type::Child;
                    childRow.guid = child->guid;
                    childRow.groupDepth = depth;   // same group as its parent asset row
                    m_rows.push_back(childRow);
                }
            }
        }

        m_rail.clear();
        int totalMatching = 0;
        for (int i = 0; i < kAssetKindCount; ++i)
            totalMatching += kindCounts[i];
        m_rail.push_back({ -1, "All", totalMatching });
        for (int i = 0; i < kAssetKindCount; ++i)
        {
            if (kindCounts[i] == 0)
                continue;   // the rail hides zero-count kinds
            m_rail.push_back({ i, KindLabel(static_cast<AssetKind>(i)), kindCounts[i] });
        }
    }

    HealthCounts AssetPanelModel::Health() const
    {
        HealthCounts h;
        h.total = static_cast<int>(m_entries.size());   // folded children included
        for (const auto& [guid, e] : m_entries)
        {
            switch (e.cook)
            {
                case CookState::Cooked:  ++h.cooked;  break;
                case CookState::Queued:  ++h.queued;  break;
                case CookState::Refused: ++h.refused; break;
                case CookState::Unknown: break;
            }
        }
        return h;
    }

    const AssetPanelEntry* AssetPanelModel::Find(const Arcane::Guid& id) const
    {
        const auto it = m_entries.find(id);
        return it == m_entries.end() ? nullptr : &it->second;
    }

    bool AssetPanelModel::Filtered() const
    {
        return !m_search.empty() || m_kindFilter != -1;
    }

    void AssetPanelModel::ResetForProjectSwitch()
    {
        m_entries.clear();
        m_dirty.clear();
        m_allDirty = true;
        m_rows.clear();
        m_rail.clear();
        m_shownAssetCount = 0;
        m_rowsDirty = true;
        m_search.clear();
        m_kindFilter = -1;
        m_groupOpen.clear();
        m_childrenOpen.clear();
        selected = Arcane::Guid{};
        selectionStamp = 0;
    }
}
