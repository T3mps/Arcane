#pragma once

// The settings window (settings arc S3, spec s6): ONE function draws either
// Editor Preferences or Project Settings from a SettingsWindowEnv --
// the category tree (left), search + filter + Show advanced (top), the
// Restart bar and the next-world note, and the selected category's page
// (right): its custom page if one is registered (which owns the node's
// keychord rows), then its rows, its child categories as sub-headers.
// Window-local undo (Ctrl+Z / Ctrl+Y while the window is focused); a game
// module's vanished category shows "Module unloaded" and repopulates on
// reload; closing flushes the archive.
// The process-wide pair and the contract entry points: SettingsHost.

#include "Scene/EditGesture.hpp"
#include "Settings/SettingsApply.hpp"
#include "Settings/SettingsEdit.hpp"
#include "Settings/SettingsModel.hpp"
#include "Settings/SettingsRows.hpp"
#include "Widgets/PropertyGrid.hpp"

#include <Arcane/Config/CVarRegistry.hpp>
#include <Arcane/Edit/CommandStack.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Arcane::Editor
{
    struct AssetRefServices;

    struct SettingsWindowEnv
    {
        CVarRegistry* registry = nullptr;
        SettingsWindowKind kind = SettingsWindowKind::Preferences;
        const char* title = "Editor Preferences";        // the ImGui window name (the dock/ini key)
        SettingsModuleRoles roles;
        std::vector<SettingsPageRef> pages;                // this window's custom pages
        std::function<void(const std::string& path)> drawPage;   // draws the page registered at `path`
        std::function<double()> now;                       // seconds, monotonic
        SettingsArchiveQueue* archive = nullptr;
        RungWriter writeRung;
        SettingsApplyTracker* tracker = nullptr;
        std::function<void()> restartEditor;               // null: the bar's button is disabled
        std::string restartBlockedReason;                  // non-empty: disabled, with this tooltip
        const AssetRefServices* assetRefs = nullptr;
        std::function<void(const std::string& cvar, bool folder)> browsePath;
        bool projectOpen = true;
    };

    struct SettingsWindowState
    {
        SettingsModel model;
        std::vector<SettingsPageRef> builtPages;           // pages at the model's last rebuild
        SettingsModuleRoles builtRoles;                    // roles at the model's last rebuild
        PropertyGridState grid;
        std::unique_ptr<Arcane::CommandStack> undo;        // window-local (spec s6.3); SettingsUndo builds it
        EditGesture::GestureState gesture;
        SettingsGestureMemo memo;
        std::unordered_map<std::string, std::string> textDrafts;
        std::string selected;                              // the selected node's path
        char search[128] = {};
        SettingsModel::Filter filter = SettingsModel::Filter::All;
        bool showAdvanced = false;
        bool wasOpen = false;
        bool focused = false;                              // root-and-child focus, this frame
        // TEST SEAM: what the last frame decided.
        struct FrameFacts
        {
            enum class Page : std::uint8_t { None, Rows, Custom, ModuleUnloaded } page = Page::None;
            std::vector<std::string> treePaths, rows, overridden, groups;
            std::size_t restartPending = 0, nextWorldPending = 0;
            std::string tooltip, contextMenu;
        } last;
    };

    [[nodiscard]] Arcane::CommandStack& SettingsUndo(SettingsWindowState& state);
    void DrawSettingsWindow(SettingsWindowState& state, const SettingsWindowEnv& env, bool* open);

    // For a custom page's draw function ONLY (RegisterSettingsPage): the
    // page's PropertyGrid, and one standard row (same widgets, provenance
    // and undo as the generated rows) -- call it inside a PropertyGrid::Rows.
    [[nodiscard]] PropertyGrid& CurrentSettingsGrid();
    bool SettingsRow(std::string_view name);
}
