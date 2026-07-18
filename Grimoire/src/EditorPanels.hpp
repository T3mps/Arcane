#pragma once

#include "ViewportInput.hpp"
#include <cstdint>

namespace Arcane { class RunLoop; }
namespace Astra { class Registry; }

namespace Grimoire
{
    class ConsoleBuffer;
    struct SelectionContext;

    // Host a full-viewport dockspace (call once per frame between ImGui BeginFrame
    // and the panel Begin/End calls). Enables initial docking of child windows.
    void BeginDockSpace();

    // Play/Pause/Step buttons + a time-scale slider, driving the RunLoop.
    void DrawSimTimeToolbar(Arcane::RunLoop& loop);

    // Scrolling read-only console of captured log lines (autoscroll).
    void DrawConsolePanel(const ConsoleBuffer& console);

    struct ViewportPanelResult
    {
        ViewportRect imageRect{};   // screen-space rect of the drawn image
        bool         hovered = false;
        bool         focused = false;
        uint32_t     desiredW = 0;  // content-region size (for OffscreenCanvas::Resize)
        uint32_t     desiredH = 0;
    };

    // Draw the scene texture into a dockable Viewport window; report its rect,
    // hover/focus, and the content-region size the offscreen canvas should match.
    ViewportPanelResult DrawViewportPanel(uint64_t textureId, uint32_t texW, uint32_t texH);

    // List every live entity; clicking a row selects it. Labels rows by id
    // ("Entity <id> (v<version>)") since no Name component exists yet.
    void DrawHierarchyPanel(Astra::Registry& registry, SelectionContext& sel);

    // Show the selected entity's components (via Registry::InspectEntity) and edit
    // reflected fields in place; unsupported types render read-only.
    void DrawInspectorPanel(Astra::Registry& registry, const SelectionContext& sel);
}
