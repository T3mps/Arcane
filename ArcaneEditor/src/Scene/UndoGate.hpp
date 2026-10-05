#pragma once

// Undo gating (spec 2026-09-30 s3.3(b)/(d)). ONE Play barrier predicate is
// shared by the Edit menu, the Ctrl+Z/Y path and every document's undo
// resolver (UE: PIE start PL:2971-2972 / EndPlayMap PL:676-677). Pure: no ImGui.

#include <Arcane/Edit/CommandStack.hpp>

#include <functional>
#include <string>
#include <string_view>

namespace Arcane::Editor
{
    [[nodiscard]] constexpr bool UndoBarred(bool playing) noexcept { return playing; }

    // Settings arc S3-13: a focused settings window owns Ctrl+Z / Ctrl+Y for
    // its window-local undo (spec s6.3). The scene stack stands down while
    // that is true, and also while Play or an open transaction already bars it.
    [[nodiscard]] constexpr bool SceneConsumesUndoKeys(bool playing, bool inTransaction,
                                                       bool settingsWindowFocused) noexcept
    {
        return !UndoBarred(playing) && !inTransaction && !settingsWindowFocused;
    }

    // HandleUndoRedoAndSceneShortcuts's scene Ctrl+Z / Ctrl+Y apply. A focused
    // settings window (spec s6.3) stands the scene stack down; Play and an
    // open transaction already do. Tests drive this same function.
    inline void DispatchSceneUndoKeys(Arcane::CommandStack& scene,
                                      bool shortcutsLive,
                                      bool playing,
                                      bool inTransaction,
                                      bool settingsWindowFocused,
                                      bool undoPressed,
                                      bool redoPressed)
    {
        const bool sceneOwns = shortcutsLive
            && SceneConsumesUndoKeys(playing, inTransaction, settingsWindowFocused);
        if (sceneOwns && undoPressed) scene.Undo();
        if (sceneOwns && redoPressed) scene.Redo();
    }

    // A document's view of the shared stack, resolved per edit. Null = no undo
    // coverage now (Play): the document still edits and saves, it just pushes
    // nothing -- the "null = Play mode" contract made true.
    using UndoResolver = std::function<Arcane::CommandStack*()>;

    [[nodiscard]] inline Arcane::CommandStack* ResolveDocumentUndo(bool playing,
                                                                   Arcane::CommandStack* stack) noexcept
    {
        return UndoBarred(playing) ? nullptr : stack;
    }

    struct UndoMenuItem
    {
        bool        enabled = false;
        std::string label;
        std::string tooltip;   // empty = none (EditorPanels.cpp's File > Save disabled-tooltip pattern)
    };

    // Edit > Undo (redo = false) or Edit > Redo (redo = true). `can` =
    // CanUndo/CanRedo, `stepLabel` = UndoLabel/RedoLabel. The clear reason
    // applies to Undo only and shows while nothing is undoable.
    [[nodiscard]] inline UndoMenuItem UndoMenuState(bool can, bool playing, bool inTransaction,
                                                    std::string_view clearedReason,
                                                    std::string_view stepLabel = {}, bool redo = false)
    {
        const std::string verb = redo ? "Redo" : "Undo";
        UndoMenuItem item;
        if (!can && !redo && !clearedReason.empty())
            item.label = "Can't undo after: " + std::string(clearedReason);
        else
            item.label = (can && !stepLabel.empty()) ? verb + " " + std::string(stepLabel) : verb;
        if (UndoBarred(playing))
        {
            item.tooltip = redo ? "Stop play mode to redo" : "Stop play mode to undo";
            return item;
        }
        if (inTransaction)
        {
            item.tooltip = "Finish the current edit first";   // drafting pick, 9.28.5
            return item;
        }
        item.enabled = can;
        return item;
    }
}
