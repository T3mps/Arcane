#include <Panels/AssetPanelModel.hpp>

#include <Arcane/Project/AssetRegistry.hpp>

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace Arcane::Editor
{
    namespace
    {
        // folder = the group KEY (AssetPanelRow::groupName's own doc comment in
        // the header spells out the two shapes in full) -- MOUNT-ROOTED (spec
        // s5/s6, 2026-09-07 third revision): the "game" scheme (the project's
        // primary mount) keeps the UNQUALIFIED shape unchanged from every prior
        // revision (root files fold into the synthetic "Content/" bucket, a
        // nested dir is its own bare relative path); every OTHER scheme (e.g.
        // "diag") gets a QUALIFIED shape, "<scheme>://" + that same
        // bare-relative-path convention -- "<scheme>://" alone for a rootless
        // file (that mount's own root), "<scheme>://<relpath>/" nested. This is
        // what keeps a real "game://diagnostics/" directory's key from ever
        // colliding with "diag://"'s own root key (see GroupDepthOf's own
        // header comment for why the two key spaces are provably disjoint).
        // name = stem, fileName = stem + extension (rows show this).
        AssetPanelEntry MakeBaseEntry(const Arcane::Guid& guid, const std::string& mountPath)
        {
            AssetPanelEntry e;
            e.guid = guid;
            e.mountPath = mountPath;
            e.kind = AssetKindOf(mountPath);

            std::string scheme;
            std::string_view rest = mountPath;
            if (const std::size_t sep = rest.find("://"); sep != std::string_view::npos)
            {
                scheme = std::string(rest.substr(0, sep));
                rest = rest.substr(sep + 3);
            }

            const std::size_t slash = rest.rfind('/');
            const std::string_view file = (slash == std::string_view::npos)
                                               ? rest : rest.substr(slash + 1);
            const std::string relDir = (slash == std::string_view::npos)
                                            ? std::string()
                                            : std::string(rest.substr(0, slash + 1));

            e.folder = (scheme.empty() || scheme == "game")
                           ? (relDir.empty() ? std::string("Content/") : relDir)
                           : (scheme + "://" + relDir);

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

        // Texture/Sprite/Mesh have a real cook pipeline of their own (see
        // this function's own header comment) -- everything else (materials,
        // scenes, data, ...) has nothing to be "pending" about, so
        // this gate keeps a `pending` answer from leaking through as a
        // permanent Queued state for a kind that never cooks. This function
        // stays PURE and self-sufficient: it does not assume the host gated
        // on kind too. (EditorApp's oracle now does -- review round 1, so
        // the expensive artifact-store ask is never paid for a kind whose
        // answer this line discards -- but that is the host's performance
        // concern, not this function's correctness contract.)
        const bool cooks = (kind == AssetKind::Texture)
                        || (kind == AssetKind::Sprite)
                        || (kind == AssetKind::Mesh);
        if (!cooks)
            return CookState::Cooked;

        return pending ? CookState::Queued : CookState::Cooked;
    }

    bool IsUnusedEligible(AssetKind kind)
    {
        // Spec s9.1's list, verbatim and exhaustive -- the kinds whose
        // consumers the reference index fully sees. Everything else
        // (Scene/Data/Audio/Font/Diagnostic/Other) is EXEMPT: a scene is a
        // root, and data/audio/font assets are pulled in by game code no
        // index observes, so a zero-inbound one is not evidence of anything.
        //
        // F2c s4.1, Task 9: Model joins the list. A Model's one consumer is
        // its companion .arcmesh, whose DerivesFrom edge the reference index
        // reads (Task 12) exactly like a Sprite's DerivesFrom edge onto its
        // source Texture -- so a Model with no companion mesh is genuinely
        // unreferenced, not merely unobserved.
        return kind == AssetKind::Texture || kind == AssetKind::Material
            || kind == AssetKind::Sprite  || kind == AssetKind::Mesh
            || kind == AssetKind::Model;
    }

    bool ThumbnailEligible(AssetKind kind)
    {
        // See this function's own header comment (AssetPanelModel.hpp) for the
        // full reasoning: Material and Mesh are both real, resolvable
        // appearances MaterialPreviewHarvester knows how to render; Texture
        // resolves its own artifact thumbnail directly, never through the
        // harvester, and Model wears its companion .arcmesh's picture rather
        // than earning a second one.
        return kind == AssetKind::Material || kind == AssetKind::Mesh;
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
            m_refIndex.Clear();   // the index tracks m_entries -- never outlive them
            m_dirty.clear();
            m_allDirty = false;
            m_rows.clear();
            m_rail.clear();
            m_shownAssetCount = 0;
            m_rowsDirty = false;
            // Plan 3 Task 3: the entries + index the Graph lens projects from
            // just went away, which is exactly the change entriesStamp exists
            // to announce.
            if (hadAnything)
                ++entriesStamp;
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
                // Plan 2 Task 4: the index tracks entries, so a pruned entry
                // is retracted here in the same loop that drops it -- FOLDED
                // CHILDREN INCLUDED (they are ordinary entries, and one that
                // holds the only DerivesFrom edge to a texture is exactly the
                // referencer whose loss makes that texture unused). Update's
                // own step 2 does the rest: this guid's outbound edges are
                // retracted from every target, and its own node survives ONLY
                // if something still points at it -- i.e. as a tombstone in
                // DanglingTargets(), never as a phantom asset.
                m_refIndex.Update(it->first, /*exists=*/false, std::nullopt);
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
            // ListAssetReferences's own doc comment (Assets.hpp). A folding
            // sprite's DerivesFrom names a TEXTURE; a folding companion
            // .arcmesh's DerivesFrom names a MODEL (F2c s4.2/s8). Never
            // another material, never an arbitrary kind.
            e.isInstance = (e.kind == AssetKind::Material) && (derivesFromCount > 0);

            // Fold: exactly one DerivesFrom, and it resolves to a Texture or
            // a Model. Whitelist, not "anything with one DerivesFrom" -- a
            // fold under an arbitrary asset would put a mesh inside a scene
            // row.
            if (derivesFromCount == 1)
            {
                const auto itT = live.find(derivesFromTarget);
                if (itT != live.end())
                {
                    const AssetKind targetKind = AssetKindOf(*itT->second);
                    if (targetKind == AssetKind::Texture || targetKind == AssetKind::Model)
                        e.foldedUnder = derivesFromTarget;
                }
            }

            // Sliced: a sprite that did NOT fold but still names a texture via
            // a References-kind ref (controller ruling, Task 4).
            e.sliced = (e.kind == AssetKind::Sprite) && !e.foldedUnder.IsValid() && hasTextureReference;

            e.cook = p.cookStateFor ? p.cookStateFor(guid) : CookState::Unknown;

            // Plan 2 Task 4: feed the reference index THE SAME `refs` optional
            // the entry build above just consumed -- THIS MODEL makes exactly
            // ONE p.refsFor ask per rebuilt guid and that must never become
            // two (the per-guid invalidation contract, pinned by the
            // call-count case in AssetPanelModelTest.cpp). The invariant is
            // scoped to the model's OWN asks: a host's COMPOSED provider may
            // legitimately add one of its own for the same guid in the same
            // rebuild -- EditorApp's cookStateFor does exactly that for a
            // SPRITE, whose cook state is derived through its texture ref
            // (FirstTextureRefOf; see IsCookPending's own cost note). That is
            // the host's cost to account for, not a break of this rule.
            // The nullopt shape is carried
            // through verbatim on purpose: it is what tells the index "could
            // not read/parse this walk", which keeps its last-known-good
            // edges instead of retracting them (spec s3.2).
            m_refIndex.Update(guid, /*exists=*/true, refs);

            m_entries[guid] = std::move(e);
        };

        if (m_allDirty)
        {
            // A full rebuild re-walks every asset, so the index is rebuilt
            // from scratch rather than incrementally patched -- this is also
            // what drops tombstones for targets nothing points at any more.
            m_refIndex.Clear();
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

            // Plan 2 Task 4 (spec s9.1): `unused` over ALL entries, not just
            // the dirtied ones. Re-pointing ONE asset changes the inbound
            // count of two OTHERS -- its old target and its new one -- and
            // neither is itself dirty, so a dirty-only pass would leave stale
            // flags on both. This is a pure map lookup per entry: the PARSE
            // is what per-guid invalidation protects (and it stayed protected
            // -- no provider is consulted here), never this flag.
            for (auto& [guid, e] : m_entries)
                e.unused = IsUnusedEligible(e.kind) && m_refIndex.InboundCount(guid) == 0;
        }

        m_dirty.clear();
        m_allDirty = false;

        if (entriesChanged)
        {
            m_rowsDirty = true;
            // Plan 3 Task 3: `entriesChanged` is set on exactly the passes
            // that touched m_entries and/or m_refIndex -- the Graph lens's two
            // inputs -- so this is the one honest place to bump the stamp. A
            // pass that only rebuilt ROWS (a search keystroke) deliberately
            // does NOT bump it: the graph does not read Rows().
            ++entriesStamp;
        }

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
        // m_groupOpen's own default -- OPEN for almost every key, except the
        // diag:// mount root (GroupDefaultOpen, spec s5 third revision) --
        // shared by the ancestor-chain walk and each folder's own toggle below,
        // so both read the identical rule.
        bool GroupOpenOrDefault(const std::unordered_map<std::string, bool>& groupOpen,
                                const std::string& folder)
        {
            const auto it = groupOpen.find(folder);
            return it == groupOpen.end() ? GroupDefaultOpen(folder) : it->second;
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
        // their parent instead (fold semantics). Keyed by the FULL group KEY
        // (AssetPanelRow::groupName's own doc comment) -- plain lexicographic
        // order over strings that all carry a trailing '/' is already a valid
        // tree PREORDER WITHIN one mount (a directory's own key is always
        // immediately followed by every one of its descendants, before any
        // later sibling subtree: '/' (0x2F) sorts below every letter/digit a
        // real path segment starts with, so "textures/" < "textures/patterns/"
        // < "textures0/" holds generally). This is what lets the single
        // sorted-iteration loop below double as the nested-group walk with no
        // separate tree structure.
        //
        // MOUNT-ROOTED (spec s6, third revision): plain lexicographic order
        // is NOT enough across mounts -- "diag://" (qualified) would sort
        // BETWEEN "Content/" and "materials/" on raw bytes ('C' < 'd' < 'm'),
        // interleaving diagnostics/ INSIDE Content/'s own subtree instead of
        // after it entirely (measured, not assumed: this is exactly what a
        // first cut produced, caught by the tracked-ReferenceProject capture
        // this pass's own report cites). `GroupKeyLess` fixes the ordering
        // withOUT touching the KEYS themselves: every unqualified (game/
        // Content) key sorts before every qualified (any other scheme) key,
        // unconditionally -- "Content/ is always first" holds regardless of
        // what a future scheme happens to be named -- and within either
        // bucket, plain lexicographic order applies exactly as before (so
        // Content/'s own subtree ordering, and the ordering WITHIN diag://'s
        // own subtree, are both bit-for-bit unchanged).
        //
        // Within the qualified bucket, the SCHEME's rank (MountSchemeRank,
        // AssetPanelModel.hpp -- user-directed 2026-09-12: Source/ above
        // diagnostics/) decides between two different mounts BEFORE the key
        // text does: on raw bytes "diag://" < "source://" would put crash
        // reports above the project's own code. Two keys of the SAME scheme
        // share a rank, so a mount's own subtree order is exactly the `a < b`
        // it always was.
        struct GroupKeyLess
        {
            static std::string_view SchemeOf(const std::string& key)
            {
                const std::size_t sep = key.find("://");
                return sep == std::string::npos ? std::string_view{}
                                                : std::string_view(key).substr(0, sep);
            }
            bool operator()(const std::string& a, const std::string& b) const
            {
                const bool aQualified = a.find("://") != std::string::npos;
                const bool bQualified = b.find("://") != std::string::npos;
                if (aQualified != bQualified)
                    return !aQualified;   // unqualified (game/Content) always first
                if (aQualified)
                {
                    const int ra = MountSchemeRank(SchemeOf(a));
                    const int rb = MountSchemeRank(SchemeOf(b));
                    if (ra != rb)
                        return ra < rb;
                }
                return a < b;
            }
        };
        std::map<std::string, std::vector<const AssetPanelEntry*>, GroupKeyLess> byFolder;

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

        // Which folders get a GROUP ROW at all. Base rule (unchanged): a
        // folder with >=1 own matching entry always gets one ("groups with
        // zero visible rows are... dropped", spec s6). PLUS (nested-groups
        // review fix round 1, Critical 1): every ANCESTOR of such a folder ALSO gets a
        // (groupCount==0) bridge row, UNCONDITIONALLY -- not gated on search.
        //
        // A kind filter (the rail, not just the search box) routinely leaves
        // an ancestor with ZERO own matching entries while a DESCENDANT still
        // has some -- e.g. the rail filtered to Materials while "fx/" holds
        // only textures and "fx/glow/" holds a material. The first cut of
        // this feature only bridged under search, which left exactly that
        // shape UNREACHABLE: with "fx/" absent from byFolder (all its own
        // entries kind-filtered out) and never bridged, "fx/glow/" existed
        // only if EVERY ancestor's m_groupOpen happened to default-true --
        // and a STALE closed flag on "fx/" (set earlier, while it still had
        // its own visible entries and its own chevron to click) hid the whole
        // subtree with NO group row left to reopen it, while the rail still
        // counted the material. Bridging always -- search or not -- means
        // every subtree keeps a chevron: still hideable by a closed ancestor
        // (AncestorsOpen below), but never UNREACHABLY so. A bridge folder's
        // own subtree is never empty by construction (it exists only because
        // some DESCENDANT has a match), so "groups with zero visible rows are
        // dropped" still holds -- a bridge row is never itself the zero case
        // that rule means to drop.
        const bool searchActive = !m_search.empty();
        // GroupKeyLess (defined above byFolder): keeps every mount's subtree
        // contiguous in iteration order -- see that struct's own comment.
        std::set<std::string, GroupKeyLess> renderFolders;
        for (const auto& [folder, vec] : byFolder)
        {
            renderFolders.insert(folder);
            std::string parent = GroupParentOf(folder);
            while (!parent.empty())
            {
                renderFolders.insert(parent);
                parent = GroupParentOf(parent);
            }
        }

        // Iterates sorted under GroupKeyLess -- the same valid-preorder-per-
        // mount property byFolder relies on above, PLUS every mount's own
        // subtree staying contiguous (Content/'s entire tree, then each other
        // populated mount's entire tree, never interleaved).
        for (const std::string& folder : renderFolders)
        {
            const int depth = GroupDepthOf(folder);
            const auto vecIt = byFolder.find(folder);
            const int ownCount = (vecIt != byFolder.end()) ? static_cast<int>(vecIt->second.size()) : 0;

            // Cascading collapse: an ancestor's closed flag hides this row (and
            // therefore everything under it, transitively, via each descendant's
            // own AncestorsOpen check) UNLESS search is active, in which case
            // this row shows unconditionally (2026-09-07 ruling, nested-groups
            // review fix round 1: search reveals matches UNIFORMLY -- collapse
            // is overridden at every level of the chain, including a row's own
            // immediate group, not just a shallower ancestor). NOTE this is
            // now independent of WHY `folder` is in renderFolders: since Critical 1's fix a bridge
            // folder (ownCount==0, present only because a descendant matches)
            // is bridged unconditionally, search or not -- `searchActive` here
            // still only controls whether COLLAPSE is bypassed, not whether the
            // row exists at all.
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
                // Nested-groups review fix round 1, Important 4 (controller ruling): search reveals
                // matches UNIFORMLY -- the fold gate is overridden under active
                // search exactly like group collapse is, above. Before this fix
                // a matching derived sprite under a COLLAPSED texture stayed
                // hidden even while the identical search revealed rows under a
                // collapsed FOLDER, which was the inconsistency the ruling
                // closed (spec s6's fold-precedent wording is corrected
                // alongside this fix -- see docs/specs/2026-09-06-asset-
                // manager-redesign-design.md s6/s17).
                if (!childrenOpen && !searchActive)
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
            if (e.unused)
                ++h.unused;
        }
        return h;
    }

    std::vector<Arcane::Guid> AssetPanelModel::UnusedGuids() const
    {
        // Sorted by NAME, mount path breaking ties -- m_entries is unordered,
        // so without a total order two same-named assets would swap places
        // between rebuilds and the Unreferenced card would visibly churn.
        // Same tie-break BuildAssetEntries already uses for the browse list.
        std::vector<const AssetPanelEntry*> flagged;
        for (const auto& [guid, e] : m_entries)
            if (e.unused)
                flagged.push_back(&e);

        std::sort(flagged.begin(), flagged.end(),
                 [](const AssetPanelEntry* a, const AssetPanelEntry* b)
                 { return a->name != b->name ? a->name < b->name : a->mountPath < b->mountPath; });

        std::vector<Arcane::Guid> out;
        out.reserve(flagged.size());
        for (const AssetPanelEntry* e : flagged)
            out.push_back(e->guid);
        return out;
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
        m_refIndex.Clear();   // a new project's reference topology shares nothing with the old
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
        // entriesStamp is deliberately NOT reset here -- see its declaration:
        // a monotonic counter can never compare equal to a stale "built at"
        // value a consumer is still holding from the outgoing project.
        ++entriesStamp;
    }
}
