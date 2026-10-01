#pragma once

// The Asset Browser's shared selection as an Inspector source (inspector
// filters spec 2026-09-29 s6): AssetPanelModel::selected -- written by the
// Browser, the Graph and the Status panels alike -- is ONE source, kind
// "assets". Its page is the old Asset Browser preview pane's content (thumb,
// identity, Derived list, action row) plus, for a texture, the import
// settings. Its Open is reported (AssetPanelActions::openAsset) and opened by
// the app's ConsumeAssetPanelActions through OpenAssetRow (s5.6). Owned by
// EditorApp; rebound every frame, like SceneInspectorSource, because what it
// draws through can be replaced by a project switch.

#include "Panels/InspectorSource.hpp"

#include <Arcane/Guid.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Arcane { class Project; }

namespace Arcane::Editor
{
    class AssetPanelModel;
    class PropertyGrid;
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

    // s5.6: one verb on the asset page's icon row. Reported, never performed:
    // `run` writes an AssetPanelActions field the app drains next frame.
    struct PageAction
    {
        const char* icon = nullptr;
        const char* tooltip = nullptr;   // today's button label ("Copy Path"): tooltip + overflow menu text
        const char* id = nullptr;        // stable "##asset_<verb>"
        bool enabled = true;
        std::function<void()> run;
    };
    // How many of `widths` fit one row of `avail` px: all when the whole run
    // fits, else the most that fit beside an overflow button of `moreWidth`.
    [[nodiscard]] std::size_t ActionsThatFit(std::span<const float> widths, float moreWidth,
                                             float spacing, float avail) noexcept;
    // The thumbnail side. compact: min(140, availX - spacing - 110, clamp(f x H,
    // floor, 140)); stacked: min(140, availX, clamp(f x H, floor, 140)).
    [[nodiscard]] float AssetPageThumbSize(bool compact, float availX, float spacing, float innerHeight,
                                           float heightFraction, float floorPx) noexcept;

    // The asset page (s5.6): header (height-aware thumb, clamped name, ellipsized
    // guid), one icon row, Derived and (textures) Import sections. Exposed for
    // the test; Draw() calls it.
    void DrawAssetPage(PropertyGrid& grid, const AssetPanelEntry& e, AssetPanelModel& model,
                       const Arcane::Project* project, const AssetPanelServices& services, AssetPanelActions& actions);
    // The icon row: buttons left to right; what does not fit moves into an
    // ICON_LC_ELLIPSIS "##asset_more" popup (BeginPopupBelow) as icon + label items.
    void DrawActionRow(std::span<const PageAction> actions);
}
