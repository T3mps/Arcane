#pragma once

// AssetGraphPanel (panel-split arc, Task 5): the Graph lens's canvas body
// and its lifecycle, extracted as pure motion out of AssetsPanel.cpp's
// DrawGraphLens/AssetsGraphProjectionIsCurrent/DestroyAssetsPanelCanvas --
// see AssetGraphPanel.cpp's own header comment for the full accounting.
// Exports four symbols: DrawAssetGraphBody (the six-arg lens-body shape
// DrawBrowseLens/DrawAssetStatusBody carry too -- Task 7 retargets `state`
// to AssetGraphPanelState&, not this task's concern),
// AssetsGraphProjectionIsCurrent (same signature it had in AssetsPanel.hpp;
// a later task deletes it), DestroyAssetGraphPanelCanvas (renamed from
// DestroyAssetsPanelCanvas -- same body, same two call sites in
// EditorApp.cpp, now repointed here) and SeedAssetGraphFocus (Task 5 round 1
// fix -- see its own comment for why the boot-scene seed had to become a
// small inline helper the HOST calls, rather than living inside the drawn
// body).

#include "Panels/AssetPanelCommon.hpp"   // AssetPanelActions/AssetPanelServices, BootSceneGuid
// AssetsPanel.hpp, included (not forward-declared): SeedAssetGraphFocus below
// is inline and touches AssetsPanelState's own fields (`graphFocus`,
// `graphFocusSeeded`), which needs the complete type, not just a reference
// declaration. No circularity -- AssetsPanel.hpp does not include this header
// back.
#include "Panels/AssetsPanel.hpp"

namespace Arcane { class Project; }

namespace Arcane::Editor
{
    class AssetPanelModel;
    class DocumentHost;

    // Seed `graphFocus` to the project's boot scene, once per project,
    // BEFORE anything reads it this frame -- Task 5 round 1 fix. The seed
    // used to run inside DrawAssetGraphBody's own preamble, which executes
    // AFTER DrawToolbar (DrawAssetsPanel's draw order): the toolbar's focus
    // combo (gated on lens==Graph) would read the STILL-unseeded
    // `graphFocus` on the one frame a project opens with the Graph lens
    // active, while the bottom bar -- which runs after the body -- read the
    // freshly-seeded value the same frame, so the toolbar said "focus:
    // everything" and the bottom bar said "focus: <boot scene>" in the same
    // frame. Exported as a small inline helper so `DrawAssetsPanel`
    // (AssetsPanel.cpp) can call it at the ORIGINAL seam -- right after
    // `ImGui::Begin`, before `DrawToolbar` -- restoring the pre-split
    // execution order in effect, not just the code's physical location.
    //
    // Idempotent per project: `graphFocusSeeded` (cleared by
    // DestroyAssetGraphPanelCanvas's project-switch reset) guards the write,
    // so a user's own "everything" pick on the toolbar combo is never
    // silently re-seeded back to the boot scene on a later frame.
    inline void SeedAssetGraphFocus(AssetsPanelState& state, const Arcane::Project* project)
    {
        if (project && !state.graphFocusSeeded)
        {
            state.graphFocus       = BootSceneGuid(project);
            state.graphFocusSeeded = true;
        }
    }

    // Draw the Graph lens body: the ax::NodeEditor canvas, its lazily-created
    // context, the built AssetGraphViewModel projection (rebuilt only when
    // the focus or the model's entries moved), layered nodes, two-layer
    // kind-coloured edges, the selection bridge, the peek tooltip with its
    // edge summary, double-click open, the unified context menu, and the
    // pin-drag "Derive Instance..." gesture with its dashed in-flight wire
    // and the canvas legend. `model`/`project` are read; every effect
    // travels through `actions`, gated by `services`; `docs` routes a
    // non-scene open the same way a Browse row's does. The boot-scene focus
    // seed does NOT happen in here -- see SeedAssetGraphFocus above, and its
    // caller in DrawAssetsPanel, for why it has to run before this body does.
    // See the definition's own comment (AssetGraphPanel.cpp) for the full
    // section-by-section accounting.
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
