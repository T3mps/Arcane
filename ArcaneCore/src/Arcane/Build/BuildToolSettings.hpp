#pragma once

// build.* (settings arc S6; inventory Part 1 "Build / Platform / Misc"). An
// explicit path to each build tool, for non-standard installs; empty (the
// default) keeps Toolchain's discovery. Toolchain's Resolve* functions read
// the block on every call (Live), so the editor and arcbuild -- which applies
// the EditorUser rung and --set before it resolves anything -- agree.
// Core code read only by the editor and arcbuild, so Audience::Editor stays
// (plan Open questions S5-S6); machine-wide (PreferencesMachine) because a
// tool path belongs to the machine, not the project. The inventory's "Dev"
// rows carry Flags(Dev).

#include <Arcane/Config/Settings.hpp>

#include <string>

namespace Arcane
{
    struct BuildToolSettings
    {
        std::string premakePath;
        std::string msbuildPath;
        std::string makePath;
        std::string ninjaPath;
        std::string ideExecutable;
    };

    ARC_REFLECT_TYPE(BuildToolSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "build", SettingScope::PreferencesMachine, ApplyMode::Live, Audience::Editor)
        ARC_REFLECT_FIELD(BuildToolSettings, premakePath)
            ARC_REFLECT_ATTR(Widget, "path:file") ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "premake5 executable that generates game-project workspaces. Empty: the SDK's ThirdParty/premake5, then PATH.")
        ARC_REFLECT_FIELD(BuildToolSettings, msbuildPath)
            ARC_REFLECT_ATTR(Widget, "path:file")
            ARC_REFLECT_ATTR(Tooltip, "MSBuild.exe that builds Visual Studio workspaces. Empty: the newest Visual Studio install (vswhere), then PATH. Set it to pin an older install.")
        ARC_REFLECT_FIELD(BuildToolSettings, makePath)
            ARC_REFLECT_ATTR(Widget, "path:file") ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "make executable for gmake workspaces. Empty: mingw32-make, then make, on PATH (Windows); make on PATH elsewhere.")
        ARC_REFLECT_FIELD(BuildToolSettings, ninjaPath)
            ARC_REFLECT_ATTR(Widget, "path:file") ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "ninja executable for Ninja workspaces. Empty: ninja on PATH.")
        ARC_REFLECT_FIELD(BuildToolSettings, ideExecutable)
            ARC_REFLECT_ATTR(Widget, "path:file") ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Tooltip, "Visual Studio devenv.exe that Open Visual Studio launches (it takes devenv's command line). Empty: the newest install (vswhere).")
    ARC_END_REFLECT_TYPE()
}
