#pragma once

// The ONE console command line (node-page phase s8.2), drawn by the editor's
// Console tab and the runtime overlay: hint, Tab completion over cvars and
// commands, Up/Down history (ConsoleModel), focus kept after Enter. Lives in
// ArcaneClient beside the ImGui layer: imgui is compiled into this DLL, so the
// widget shares the host's context.

#include <Arcane/Base/Api.hpp>
#include <Arcane/Config/ConsoleModel.hpp>

namespace Arcane
{
    // True on submit (the model already ran Submit with `permission`).
    ARCANE_API bool DrawConsoleInputLine(const char* id, ConsoleModel& model, CVarRegistry& registry,
                                         Permission permission);
}
