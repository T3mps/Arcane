#pragma once

// AssetBrowserPanel (panel-split arc, Task 6): the Browse lens's body -- the
// rail, the grouped/folded asset table (scroll-to-selection + arrow-key
// nav), the table<->preview drag splitter, and the resizable preview pane --
// extracted as pure motion out of AssetsPanel.cpp's DrawBrowseLens (renamed
// DrawAssetBrowserBody here -- Task 7 retargets its `state` parameter to
// AssetBrowserPanelState&, not this task's concern). See
// AssetBrowserPanel.cpp's own header comment for the full section-by-section
// accounting.

#include "Panels/AssetPanelCommon.hpp"   // AssetPanelActions/AssetPanelServices

namespace Arcane { class Project; }

namespace Arcane::Editor
{
    class AssetPanelModel;
    class DocumentHost;
    struct AssetsPanelState;

    // Draw the Browse lens body: the rail (all/per-kind counts + hover
    // create affordance, spec s6), the grouped/folded asset table, the
    // table<->preview drag splitter, and the resizable preview pane (thumb,
    // name/pills, path/guid/cook rows, Derived list, action buttons).
    // `model`/`project` are read; every effect travels through `actions`,
    // gated by `services`; `docs` routes a non-scene open the same way the
    // Graph lens's node double-click does (both call the SAME OpenAssetRow,
    // AssetPanelCommon.hpp).
    void DrawAssetBrowserBody(AssetsPanelState& state, AssetPanelModel& model,
                              const Arcane::Project* project, DocumentHost& docs,
                              const AssetPanelServices& services,
                              AssetPanelActions& actions);
}
