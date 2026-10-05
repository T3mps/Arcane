#pragma once

// The process-wide settings host (settings arc S3, spec s6): the two
// windows' state, their shared debounced archive and apply tracker, the
// custom-page registry, and the contract entry points. EditorApp
// configures it at every project open, ticks it once per frame, flushes it
// on project switch and exit, and calls the two Draw* functions EVERY frame
// (a closed window is a no-op that flushes on its close frame).

#include "App/DialogSlot.hpp"
#include "Settings/SettingsEdit.hpp"     // SettingsWindowKind
#include "Settings/SettingsModel.hpp"    // SettingsModuleRoles

#include <Arcane/Config/CVarTypes.hpp>

#include <cstdint>
#include <functional>
#include <string>

namespace Arcane::Editor
{
    struct AssetRefServices;

    struct SettingsHostConfig
    {
        SettingsModuleRoles roles;
        bool projectOpen = false;                                    // false: Project/User rungs are not writable
        const AssetRefServices* assetRefs = nullptr;
        std::function<void()> restartEditor;                        // the Restart bar's button (S3-14)
        std::function<std::string()> restartBlockedReason;          // "" = allowed
        std::function<void(const std::string& cvar, bool folder)> browsePath;
    };

    struct SettingsBootOpen { bool preferences = false; bool project = false; };

    void ConfigureSettingsHost(SettingsHostConfig config);   // boot + every project open; baselines the Restart rows on its first call
    void SettingsHostProjectClosing();                       // before a switch: flush, clear both windows' undo
    // SwitchProject's outgoing-project boundary: an accepted switch flushes
    // pending edits into the outgoing folders and clears window-local undo;
    // a refused switch (rival lock, invalid project, dirty documents) leaves
    // the host untouched. SwitchProject calls this on every exit of the
    // pre-teardown gate, including the refusals.
    void CloseSettingsHostIfProjectSwitchAccepted(bool accepted);
    [[nodiscard]] bool SettingsHostArchivePending();         // dirty rungs waiting for debounce / flush
    void TickSettingsHost();                                 // once per frame: the debounced write
    void FlushSettingsArchives();                            // editor exit
    void SettingsWorldCreated();                             // Play started / scene opened or new: NextWorld baselines
    [[nodiscard]] bool SettingsWindowFocused();              // a settings window had focus last frame
    void SelectSettingsCategory(SettingsWindowKind kind, std::string path);
    [[nodiscard]] std::string SelectedSettingsCategory(SettingsWindowKind kind);
    // editor.settings.openAtBoot ("preferences" | "project" | "both") and
    // editor.settings.openCategory, read now (automation: headless desk captures).
    [[nodiscard]] SettingsBootOpen ConsumeSettingsOpenAtBoot();
    // A Browse dialog's result for a path:file / path:dir row (EditorApp's DialogSlot).
    void ApplySettingsPathPick(const std::string& cvar, const std::string& path);
    // BrowseSettingsPath's non-OS half: remember which cvar the next pick
    // answers and Arm the settingsPath slot. Returns the epoch PathPickedThunk
    // Stashes into. The OS picker itself stays in EditorApp (needs a window).
    [[nodiscard]] std::uint64_t BeginSettingsPathBrowse(std::string& cvarSlot, DialogSlot<std::string>& slot,
                                                        const std::string& cvar);
    // The frame consume site (EditorAppFrame): Take the slot and ApplySettingsPathPick.
    void ConsumeSettingsPathPick(std::string& cvarSlot, DialogSlot<std::string>& slot);

    // ---- the contract (settings arc S3) ----
    using SettingsPageDrawFn = void (*)(void* user);
    // A page at `categoryPath` in the window `scope` belongs to (Preferences
    // = PreferencesMachine/PreferencesProject, Project). Re-registering the
    // same (window, path) replaces it; a null `fn` keeps the node, draws nothing.
    void RegisterSettingsPage(SettingScope scope, std::string categoryPath, std::string title, SettingsPageDrawFn, void* user);
    void DrawEditorPreferences(bool* open);
    void DrawProjectSettings(bool* open);
}
