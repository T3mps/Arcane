#pragma once

// The theme page (settings arc S4, spec s7.1): Preferences > Appearance > Theme.
// Presets, import/export, grouped swatches, a live preview and the contrast
// report. Edits write editor.theme.* at the EditorUser rung and publish; the
// AppearanceApplier re-themes at the next frame boundary.

#include <imgui.h>

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace Arcane::Editor
{
    struct ThemePageState
    {
        std::function<void()> requestImport;   // the app opens an OS picker; its pick lands in ImportThemeFrom
        std::function<void()> requestExport;   // ... and in ExportThemeTo
        std::string status;                    // the last preset/import/export outcome
        char previewText[64] = "Sample text";
        bool previewToggle = true;
    };

    void DrawThemePage(void* user);   // RegisterSettingsPage callback; user = ThemePageState*

    bool ApplyThemePresetByName(ThemePageState& state, std::string_view presetName);
    bool ImportThemeFrom(ThemePageState& state, const std::filesystem::path& path);
    bool ExportThemeTo(ThemePageState& state, std::filesystem::path path);   // appends .arctheme when missing
    bool SetThemeToken(std::string_view field, const ImVec4& display);       // EditorUser + Publish
}
