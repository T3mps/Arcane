#include "Settings/AssetBrowserSettings.hpp"

#include <Arcane/Config/Settings.hpp>
#include <Arcane/Reflection.hpp>

namespace Arcane::Editor
{
    ARC_REFLECT_TYPE(AssetBrowserSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "editor.assets", SettingScope::PreferencesMachine, ApplyMode::Live, Audience::Editor)
        ARC_REFLECT_FIELD(AssetBrowserSettings, railWidth)
            ARC_REFLECT_ATTR(DisplayName, "Rail width") ARC_REFLECT_ATTR(Category, "Layout")
            ARC_REFLECT_ATTR(Range, 80.0, 600.0)
            ARC_REFLECT_ATTR(Keywords, "kinds sidebar column px")
            ARC_REFLECT_ATTR(Tooltip, "Width, in pixels, of the Asset Browser's asset-kind rail.")
        ARC_REFLECT_FIELD(AssetBrowserSettings, namedTargets)
            ARC_REFLECT_ATTR(DisplayName, "Peek named targets") ARC_REFLECT_ATTR(Category, "Layout")
            ARC_REFLECT_ATTR(Range, 1.0, 20.0) ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Keywords, "tooltip references outbound")
            ARC_REFLECT_ATTR(Tooltip, "How many referenced assets an asset's peek tooltip names before folding the "
                                      "rest into \"+N\".")
        ARC_REFLECT_FIELD(AssetBrowserSettings, activityLogCapacity)
            ARC_REFLECT_ATTR(DisplayName, "Activity log entries") ARC_REFLECT_ATTR(Category, "Activity")
            ARC_REFLECT_ATTR(Range, 10.0, 10000.0) ARC_REFLECT_ATTR(Apply, ApplyMode::Restart)
            ARC_REFLECT_ATTR(Keywords, "history feed ring timeline")
            ARC_REFLECT_ATTR(Tooltip, "How many asset activity entries (imports, moves, refusals) the session keeps "
                                      "before the oldest drop.")
        ARC_REFLECT_FIELD(AssetBrowserSettings, newMaterialDefaultSurface)
            ARC_REFLECT_ATTR(DisplayName, "New material surface") ARC_REFLECT_ATTR(Category, "Create")
            ARC_REFLECT_ATTR(Range, 0.0, 2.0) ARC_REFLECT_ATTR(Scope, SettingScope::PreferencesProject)
            ARC_REFLECT_ATTR(Keywords, "sprite mesh post fullscreen create dialog")
            ARC_REFLECT_ATTR(Tooltip, "The surface the Create Material dialog starts on when nothing picked one: "
                                      "0 = sprite, 1 = mesh, 2 = post (fullscreen).")
        ARC_REFLECT_FIELD(AssetBrowserSettings, watchPollSeconds)
            ARC_REFLECT_ATTR(DisplayName, "Asset watch poll") ARC_REFLECT_ATTR(Category, "Polling")
            ARC_REFLECT_ATTR(Range, 0.1, 30.0)
            ARC_REFLECT_ATTR(Keywords, "hot reload latency seconds interval mtime")
            ARC_REFLECT_ATTR(Tooltip, "Seconds between checks for edited material and texture sources. Lower reloads "
                                      "sooner at the cost of more disk reads.")
        ARC_REFLECT_FIELD(AssetBrowserSettings, discoveryPollSeconds)
            ARC_REFLECT_ATTR(DisplayName, "Content discovery poll") ARC_REFLECT_ATTR(Category, "Polling")
            ARC_REFLECT_ATTR(Range, 0.5, 60.0)
            ARC_REFLECT_ATTR(Keywords, "new files import seconds interval scan")
            ARC_REFLECT_ATTR(Tooltip, "Seconds between scans of Content/ for new source files (.png, .gltf, .glb) to "
                                      "import.")
        ARC_REFLECT_FIELD(AssetBrowserSettings, mountDiagnostics)
            ARC_REFLECT_ATTR(DisplayName, "Mount diag://") ARC_REFLECT_ATTR(Category, "Project")
            ARC_REFLECT_ATTR(Apply, ApplyMode::NextWorld) ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Scope, SettingScope::PreferencesProject)
            ARC_REFLECT_ATTR(Keywords, "diagnostics crash reports arcdiag mount")
            ARC_REFLECT_ATTR(Tooltip, "Mount this machine's crash and hang reports as diag:// when a project opens, "
                                      "so the Asset Browser lists them. Takes effect at the next project open.")
    ARC_END_REFLECT_TYPE()

    ARC_REFLECT_TYPE(AssetStatusSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "editor.assetStatus", SettingScope::PreferencesMachine, ApplyMode::Live, Audience::Editor)
        ARC_REFLECT_FIELD(AssetStatusSettings, rightColumnMaxFraction)
            ARC_REFLECT_ATTR(DisplayName, "Right column share")
            ARC_REFLECT_ATTR(Range, 0.2, 0.8) ARC_REFLECT_ATTR(Flags, CVarFlags::Dev)
            ARC_REFLECT_ATTR(Keywords, "timeline width fraction")
            ARC_REFLECT_ATTR(Tooltip, "The most of the Asset Status panel's width its right (timeline) column may "
                                      "take.")
    ARC_END_REFLECT_TYPE()

    ARC_SETTINGS(AssetBrowserSettings);
    ARC_SETTINGS(AssetStatusSettings);

    ProjectOpenOptions EditorOpenOptions(ProjectOpenOptions hostRule)
    {
        hostRule.mountDiagnostics = hostRule.mountDiagnostics && Arcane::Settings<AssetBrowserSettings>().mountDiagnostics;
        return hostRule;
    }
}
