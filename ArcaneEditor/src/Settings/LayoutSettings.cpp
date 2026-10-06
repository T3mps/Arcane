#include "Settings/LayoutSettings.hpp"

namespace Arcane::Editor
{
    ARC_CVAR(cvar_layoutDefault, "editor.layout.default", std::string, std::string{},
             .flags = Arcane::CVarFlags::Archive, .audience = Arcane::Audience::Editor,
             .scope = Arcane::SettingScope::PreferencesMachine, .apply = Arcane::ApplyMode::Restart,
             .help = "The named layout a project opens with the first time (empty = the factory layout). Layouts are files in the Layouts folder.",
             .displayName = "Default layout", .keywords = "layout dock panels workspace");

    ARC_CVAR(cvar_layoutOpenPanelsAtStart, "editor.layout.openPanelsAtStart", std::string, std::string("*"),
             .flags = Arcane::CVarFlags::Archive, .audience = Arcane::Audience::Editor,
             .scope = Arcane::SettingScope::PreferencesMachine, .apply = Arcane::ApplyMode::Restart,
             .help = "Panels shown on a fresh or reset layout: * for all, or a comma list of panel names (Outliner, Inspector, Asset Browser, Asset Graph, Asset Status, Problems, Console).",
             .displayName = "Panels at start", .keywords = "layout panels windows visible open");
}
