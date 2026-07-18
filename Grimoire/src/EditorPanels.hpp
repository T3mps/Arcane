#pragma once

namespace Arcane { class RunLoop; }

namespace Grimoire
{
    class ConsoleBuffer;

    // Host a full-viewport dockspace (call once per frame between ImGui BeginFrame
    // and the panel Begin/End calls). Enables initial docking of child windows.
    void BeginDockSpace();

    // Play/Pause/Step buttons + a time-scale slider, driving the RunLoop.
    void DrawSimTimeToolbar(Arcane::RunLoop& loop);

    // Scrolling read-only console of captured log lines (autoscroll).
    void DrawConsolePanel(const ConsoleBuffer& console);
}
