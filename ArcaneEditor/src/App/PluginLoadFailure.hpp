#pragma once

// The modal a failed game-module / plugin load raises (EditorApp.cpp,
// StagePluginLoad; SwitchProject reuses that body). It names what was
// actually attempted: with a project open, the manifest's DLLs; with none,
// the bare --plugin path.

#include <string>

namespace Arcane::Editor
{
    struct ModalText
    {
        std::string title;
        std::string body;
    };

    [[nodiscard]] inline ModalText DescribePluginLoadFailure(bool projectOpen, const std::string& gameModule, int abi)
    {
        const std::string abiText = std::to_string(abi);
        if (projectOpen)
            return { "Open Project Failed",
                     "The project opened, but its game module / plugins failed to load (see Console).\n"
                     "Check the DLL paths in the manifest and that they are built against ABI " + abiText + "." };
        return { "Game Module Failed to Load",
                 "--plugin '" + gameModule + "' failed to load (see Console). No project is open, so a module "
                 "that needs its project's content may refuse here: open the project instead.\n"
                 "Check that the DLL exists and is built against ABI " + abiText + "." };
    }
}
