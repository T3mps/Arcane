#pragma once

// AssetStatusPanel (panel-split arc): the "Asset Status" window -- the
// dashboard body (four health tiles, the cook-pipeline meter, then the
// two-column split spec s9.2 pins) and its bottom bar. Task 4 moved the body
// here as pure motion out of AssetsPanel.cpp's DrawStatusLens; Task 7 wrapped
// it in its own panel shell.
//
// NO STATE STRUCT (spec s6): the dashboard is entirely derived from the model
// and the services. By Task 3 every former `state` use inside this body was
// already a deep-link WRITE (the Unreferenced card's Reveal, the Scenes
// card's Focus in Graph) promoted to `actions`, so nothing here ever needed
// session state to READ -- which is why the two entry points below are the
// only ones in this arc that take no state parameter.
//
// NO TOOLBAR either (spec s9.1): the stat tiles start at the top of the body,
// and there is no toolbar-body gap because there is no toolbar to gap from.

#include "Panels/AssetPanelCommon.hpp"   // AssetPanelActions/AssetPanelServices

namespace Arcane { class Project; }

namespace Arcane::Editor
{
    class AssetPanelModel;
    class DocumentHost;

    // Draw the Status dashboard: four health tiles, the cook-pipeline
    // meter, then the two-column split (Needs attention/Unreferenced left,
    // Activity/Scenes right) spec s9.2 pins. `model`/`project` are read
    // only; `docs` is unused (Status opens nothing through the document
    // host) but kept for the shared body shape; every effect this body
    // performs travels through `actions`, gated by `services`. Called only
    // by DrawAssetStatusPanel below, which owns the window and the bottom bar.
    void DrawAssetStatusBody(AssetPanelModel& model, const Arcane::Project* project,
                             DocumentHost& docs, const AssetPanelServices& services,
                             AssetPanelActions& actions);

    // Draw the "Asset Status" window (panel-split spec s5/s9): no toolbar ·
    // body · bottom bar ("N assets - M need attention" left; the activity
    // ring's recency line, spec s9.3, right -- empty when the ring is null
    // or empty, the bar never fabricates a fact it does not have). `model`
    // is rebuilt by the caller before this runs every frame; this panel only
    // reads it. `open` is forwarded to ImGui::Begin (the tab's X button;
    // null = no X).
    AssetPanelActions DrawAssetStatusPanel(AssetPanelModel& model, const Arcane::Project* project,
                                           DocumentHost& docs, const AssetPanelServices& services,
                                           bool* open = nullptr);
}
