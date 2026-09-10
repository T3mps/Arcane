#pragma once

// AssetStatusPanel (panel-split arc, Task 4): the Status lens's dashboard
// body, extracted as pure motion out of AssetsPanel.cpp's DrawStatusLens.
// Exports exactly ONE symbol -- DrawAssetStatusBody -- the same six-arg
// lens-body shape DrawBrowseLens/DrawGraphLens still carry in
// AssetsPanel.cpp, minus `state`: spec s6 gives Status no state struct at
// all, and by Task 3 every former `state` use inside this body was already
// a deep-link WRITE (the Unreferenced card's Reveal, the Scenes card's
// Focus in Graph) promoted to `actions`, so nothing here ever needed
// session state to READ. No Begin/End of its own panel window and no
// bottom bar -- Task 7 wraps this body in its own panel shell the same way
// it will wrap Browse/Graph.

#include "Panels/AssetPanelCommon.hpp"   // AssetPanelActions/AssetPanelServices

namespace Arcane::Editor
{
    class AssetPanelModel;
    class DocumentHost;

    // Draw the Status dashboard: four health tiles, the cook-pipeline
    // meter, then the two-column split (Needs attention/Unreferenced left,
    // Activity/Scenes right) spec s9.2 pins. `model`/`project` are read
    // only; `docs` is unused (Status opens nothing through the document
    // host) but kept for the shared lens-body shape; every effect this body
    // performs travels through `actions`, gated by `services`.
    void DrawAssetStatusBody(AssetPanelModel& model, const Arcane::Project* project,
                             DocumentHost& docs, const AssetPanelServices& services,
                             AssetPanelActions& actions);
}
