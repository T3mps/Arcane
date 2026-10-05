#pragma once

// One settings row (settings arc S3, spec s6.2-s6.4), drawn with the
// PropertyGrid rows: the value widget by type and widget hint; single-shot
// edits push one SettingEditCommand onto the WINDOW's undo stack; drags and
// colour popups apply live and close as ONE step through EditGesture (the
// EditGesture-after-row contract, PropertyGrid.hpp:25-40). An overridden
// row is read-only (spec s12). S3-9 adds the decorations.

#include "Scene/EditGesture.hpp"
#include "Settings/SettingsEdit.hpp"
#include "Widgets/PropertyGrid.hpp"

#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Edit/CommandStack.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace Arcane::Editor
{
    struct AssetRefServices;

    enum class RowWidget : std::uint8_t { Checkbox, Int, IntText, Float, Slider, Double, Text, Asset, Path, Color, Vec, Enum, KeyChord, Font };
    [[nodiscard]] RowWidget RowWidgetFor(const CVarDescInfo& desc) noexcept;
    // "asset:<kind>" -> that AssetKind (KindLabel, case- and space-insensitive); -1 = any / not an asset hint.
    [[nodiscard]] int AssetKindFilterFor(std::string_view widget);

    // The live gesture's rung as it was at activation: Escape restores it exactly.
    struct SettingsGestureMemo
    {
        std::string name;
        SetBy rung = SetBy::Project;
        std::optional<CVarValue> before;
        bool live = false;
    };

    struct SettingsRowContext
    {
        CVarRegistry& registry;
        SettingsWindowKind window;
        PropertyGrid& grid;
        Arcane::CommandStack& undo;               // the window-local stack (spec s6.3)
        EditGesture::GestureState& gesture;
        SettingsGestureMemo& memo;
        const SettingsEditSink& sink;
        std::unordered_map<std::string, std::string>& textDrafts;   // per cvar, while a text box is active
        const AssetRefServices* assetRefs = nullptr;                 // null: asset rows draw as text
        std::function<void(const std::string& cvar, bool folder)> browsePath;   // null: no Browse button
        bool projectOpen = true;                  // false: Project and User-rung rows are read-only
        // TEST SEAMS, written by the rows this frame (S3-9):
        std::string lastTooltip;
        std::string lastContextMenu;
    };

    struct SettingRowResult
    {
        bool drawn = false;
        bool committed = false;
        bool overridden = false;
        bool modified = false;
        bool readOnly = false;
    };

    SettingRowResult DrawSettingRow(SettingsRowContext& ctx, std::string_view name);
}
