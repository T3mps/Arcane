#pragma once

// Theme presets and .arctheme files (settings arc S4, spec s7.1). Files carry
// display sRGB "#rrggbbaa" keyed by the full cvar name; applying one writes
// the EditorUser rung (machine-wide) for each key it names.

#include "Settings/EditorThemeSettings.hpp"

#include <array>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Arcane { class CVarRegistry; }

namespace Arcane::Editor
{
    struct ThemeFile
    {
        std::string name;
        std::vector<std::pair<std::string, ImVec4>> colors;   // (field, display colour), file order
        std::vector<std::string> unknownKeys;                  // reported, never applied
    };

    inline constexpr std::array<std::string_view, 3> kThemePresetNames = { "Dark", "Light", "HighContrast" };

    [[nodiscard]] std::optional<ImVec4> ParseHexColor(std::string_view text);   // "#rrggbb" or "#rrggbbaa"
    [[nodiscard]] std::string FormatHexColor(const ImVec4& c);                   // "#rrggbbaa", ImGui's 8-bit rounding
    [[nodiscard]] std::expected<ThemeFile, std::string> ReadThemeFile(const std::filesystem::path& path);
    bool WriteThemeFile(const std::filesystem::path& path, std::string_view name, const Theme::Palette& palette,
                        std::string* error = nullptr);
    [[nodiscard]] EditorThemeSettings ApplyThemeFileTo(const ThemeFile& file, EditorThemeSettings base);
    // Sets editor.theme.<field> at SetBy::EditorUser for each colour; returns how
    // many were applied. The caller publishes.
    std::size_t ApplyThemeToRegistry(const ThemeFile& file, Arcane::CVarRegistry& registry);
    [[nodiscard]] std::filesystem::path ThemePresetDir();   // <exe dir>/data/EditorThemes

    struct ContrastRow
    {
        std::string_view label;
        float ratio = 0.0f;
        float minRatio = 0.0f;
        bool ok = false;
    };
    [[nodiscard]] std::vector<ContrastRow> ContrastReport(const Theme::Palette& palette);
}
