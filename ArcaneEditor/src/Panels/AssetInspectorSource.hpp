#pragma once

// The Asset Browser's shared selection as an Inspector source (inspector
// filters spec 2026-09-29 s6): AssetPanelModel::selected -- written by the
// Browser, the Graph and the Status panels alike -- is ONE source, kind
// "assets". Its page is the old Asset Browser preview pane's content (thumb,
// identity, Derived list, action buttons) plus, for a texture, the import
// settings. Owned by EditorApp; rebound every frame, like
// SceneInspectorSource, because what it draws through can be replaced by a
// project switch.

#include "Panels/InspectorSource.hpp"

#include <Arcane/Guid.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Arcane { class Project; }

namespace Arcane::Editor
{
    class AssetPanelModel;
    class DocumentHost;
    struct AssetPanelActions;
    struct AssetPanelEntry;
    struct AssetPanelServices;

    class AssetInspectorSource final : public InspectorSource, public InspectorPage
    {
    public:
        struct Deps
        {
            AssetPanelModel*          model = nullptr;
            const Arcane::Project*    project = nullptr;
            DocumentHost*             docs = nullptr;       // Open button (OpenAssetRow)
            const AssetPanelServices* services = nullptr;   // thumbnails + peek tooltips
            AssetPanelActions*        actions = nullptr;    // the page's clicks; the app drains them next frame
        };
        void Bind(const Deps& deps) { m_deps = deps; }

        // InspectorSource
        std::string SourceName() const override { return "Assets"; }
        std::string_view Kind() const override { return "assets"; }
        InspectorPage* Page() override;
        InspectorPage* PageFor(std::string_view key) override;
        std::string SelectionKey() const override;            // model->selected.ToString(), "" when invalid
        bool RestoreSelection(std::string_view key) override; // Select(guid) when Find(guid) resolves
        bool Resolves(std::string_view key) const override;   // Find(guid) != nullptr
        [[nodiscard]] std::uint64_t SelectionEpoch() const;   // model->selectionGesture (0 unbound)

        // InspectorPage
        std::vector<InspectorCrumb> Breadcrumb() const override;  // "Assets" (select: clears) > <fileName> (key = guid)
        void Draw(PropertyGrid& grid) override;

    private:
        Deps m_deps{};
        Arcane::Guid m_drawGuid;   // what Draw/Breadcrumb read: Page() -> selected, PageFor(key) -> key
    };

    // The thumbnail + identity + Derived list + action buttons + (texture) import
    // settings -- the old Asset Browser preview pane, now an Inspector page.
    // Exposed for the test; Draw() calls it.
    void DrawAssetPage(const AssetPanelEntry& e, AssetPanelModel& model, const Arcane::Project* project,
                       DocumentHost* docs, const AssetPanelServices& services, AssetPanelActions& actions);
}
