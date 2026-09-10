#pragma once

// AssetGraphPanel (panel-split arc, Task 5): the Graph lens's canvas body
// and its lifecycle, extracted as pure motion out of AssetsPanel.cpp's
// DrawGraphLens/AssetsGraphProjectionIsCurrent/DestroyAssetsPanelCanvas --
// see AssetGraphPanel.cpp's own header comment for the full accounting.
// Exports exactly three symbols: DrawAssetGraphBody (the six-arg lens-body
// shape DrawBrowseLens/DrawAssetStatusBody carry too -- Task 7 retargets
// `state` to AssetGraphPanelState&, not this task's concern),
// AssetsGraphProjectionIsCurrent (same signature it had in AssetsPanel.hpp;
// a later task deletes it) and DestroyAssetGraphPanelCanvas (renamed from
// DestroyAssetsPanelCanvas -- same body, same two call sites in
// EditorApp.cpp, now repointed here).

#include "Panels/AssetPanelCommon.hpp"   // AssetPanelActions/AssetPanelServices

namespace Arcane { class Project; }

namespace Arcane::Editor
{
    class AssetPanelModel;
    class DocumentHost;
    // AssetsPanel.hpp -- forward-declared rather than included, same reason
    // AssetPanelCommon.hpp's own forward declare gives: only a reference
    // parameter is needed below, and AssetGraphPanel.cpp (which has the real
    // definition to work with) includes AssetsPanel.hpp itself.
    struct AssetsPanelState;

    // Draw the Graph lens body: the ax::NodeEditor canvas, its lazily-created
    // context, the built AssetGraphViewModel projection (rebuilt only when
    // the focus or the model's entries moved), layered nodes, two-layer
    // kind-coloured edges, the selection bridge, the peek tooltip with its
    // edge summary, double-click open, the unified context menu, and the
    // pin-drag "Derive Instance..." gesture with its dashed in-flight wire
    // and the canvas legend. `model`/`project` are read (plus the boot-scene
    // focus seed, on the first frame of a project); every effect travels
    // through `actions`, gated by `services`; `docs` routes a non-scene open
    // the same way a Browse row's does. See the definition's own comment
    // (AssetGraphPanel.cpp) for the full section-by-section accounting.
    void DrawAssetGraphBody(AssetsPanelState& state, AssetPanelModel& model,
                            const Arcane::Project* project, DocumentHost& docs,
                            const AssetPanelServices& services,
                            AssetPanelActions& actions);

    // Is the Graph lens's cached projection CURRENT -- built for the focus
    // and the entries the panel would name this frame? See the definition's
    // own comment (AssetGraphPanel.cpp) for the three-conjunct rationale and
    // why this is exported rather than left file-local: the panel's bottom
    // bar (AssetsPanel.cpp) and the device-less canvas test must ask the
    // SAME question.
    [[nodiscard]] bool AssetsGraphProjectionIsCurrent(const AssetsPanelState& state,
                                                       const AssetPanelModel& model);

    // Tear the Graph lens's canvas context down and drop the built
    // projection with it (Plan 3 Task 3; renamed from DestroyAssetsPanelCanvas
    // in Task 5, panel-split). The HOST calls this at exactly two seams -- a
    // project switch and shutdown -- see the definition's own comment
    // (AssetGraphPanel.cpp) for why a function here rather than an
    // `ed::DestroyEditor` at those call sites, and for the MUST-run-while-an-
    // ImGui-context-is-current rule. Idempotent, and safe when the lens was
    // never opened.
    void DestroyAssetGraphPanelCanvas(AssetsPanelState& state);
}
