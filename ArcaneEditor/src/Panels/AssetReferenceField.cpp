#include "Panels/AssetReferenceField.hpp"

#include <Arcane/Project/Project.hpp>

#include <algorithm>

namespace Arcane::Editor
{
    AssetRefDisplay DescribeAssetRef(const AssetRefArgs& args, const AssetRefServices& services)
    {
        AssetRefDisplay d;
        if (args.kindFilter >= 0 && args.kindFilter < kAssetKindCount)
            d.kind = static_cast<AssetKind>(args.kindFilter);
        // Mixed FIRST, an Identity guid included: a multi-selection's ids always
        // differ, and the replaced arm read "--" for them (InspectorView.cpp's
        // refMixed branch ran before its identity check). Showing the primary's
        // id would present one entity's identity as the whole selection's.
        if (args.mixed) { d.text = "--"; return d; }
        // Identity guids are not references: the registry can never resolve
        // one, so asking would flag every healthy entity "(missing)".
        if (args.identityGuid)
        {
            d.text = args.guid.IsValid() ? args.guid.ToString() : std::string("(none)");
            d.kind = AssetKind::Other;
            return d;
        }
        if (!args.guid.IsValid()) { d.text = "(none)"; return d; }
        const Arcane::Project* project = services.project ? services.project() : nullptr;
        if (!project) { d.text = args.guid.ToString(); return d; }   // null services: raw guid, never dangling
        if (const std::optional<std::string> mount = project->Registry().Resolve(args.guid))
        {
            d.tooltip = *mount;
            d.browsable = true;
            if (const AssetPanelEntry* e = services.model ? services.model->Find(args.guid) : nullptr)
            {
                d.text = e->fileName;
                d.kind = e->kind;
            }
            else
            {
                // The model has not seen the guid yet (rebuilt next frame).
                const std::size_t slash = mount->rfind('/');
                d.text = slash == std::string::npos ? *mount : mount->substr(slash + 1);
                d.kind = AssetKindOf(*mount);
            }
            return d;
        }
        // Dangling: what it was called (T5's tombstone) else the raw guid.
        const std::optional<std::string> tomb =
            services.tombstoneName ? services.tombstoneName(args.guid) : std::nullopt;
        d.text = (tomb ? *tomb : args.guid.ToString()) + " (missing)";
        if (tomb) d.tooltip = args.guid.ToString() + " (missing)";
        d.dangling = true;
        return d;
    }

    AssetRefDropVerdict DecideAssetRefDrop(const AssetDragPayload& payload, const AssetRefArgs& args)
    {
        // Read-only means read-only on every path in: drag-drop acceptance
        // never consults ImGuiItemFlags_Disabled (InspectorView.cpp's history:
        // an Identity::id drop once wrote an asset guid into every selected
        // entity).
        if (args.readOnly || args.identityGuid) return AssetRefDropVerdict::Refuse;
        if (args.kindFilter < 0 || static_cast<int>(payload.kind) == args.kindFilter) return AssetRefDropVerdict::Set;
        if (args.kindFilter == static_cast<int>(AssetKind::Sprite) && payload.kind == AssetKind::Texture
            && args.allowTextureMint)
            return AssetRefDropVerdict::MintSprite;
        return AssetRefDropVerdict::Refuse;
    }

    std::vector<const AssetPanelEntry*> AssetRefCandidates(const AssetPanelModel& model, int kindFilter,
                                                           int surfaceFilter, std::string_view search)
    {
        std::vector<const AssetPanelEntry*> out;
        for (const auto& [guid, e] : model.Entries())
        {
            if (kindFilter >= 0 && static_cast<int>(e.kind) != kindFilter) continue;
            // "Show what we know, say nothing about what we don't": exclude only
            // a CONFIRMED differing surface (InspectorView.cpp:1241-1283's rule).
            if (surfaceFilter >= 0 && e.kind == AssetKind::Material && e.surface
                && static_cast<int>(*e.surface) != surfaceFilter)
                continue;
            if (!search.empty() && !MatchesFilter(AssetEntry{ e.guid, e.mountPath, e.name, e.kind }, -1, search))
                continue;
            out.push_back(&e);
        }
        std::sort(out.begin(), out.end(), [](const AssetPanelEntry* a, const AssetPanelEntry* b)
                  { return a->name != b->name ? a->name < b->name : a->mountPath < b->mountPath; });
        return out;
    }
}
