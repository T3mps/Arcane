#pragma once

// The editor's keyboard actions (settings arc S4, spec s7.2; inventory Part 3
// "Shortcuts"). Registration order is the shortcuts page's order. Fly keys and
// the QWER tool row are PHYSICAL (positional clusters); everything else is
// LABELLED.

#include "Input/EditorActions.hpp"

#include <array>

namespace Arcane::Editor
{
    inline constexpr std::array<EditorActionDesc, 50> kEditorActionTable = { {
        // Global (the SDL route, EditorAppFrame's input phase)
        { "edit.undo",                 "Undo",                ActionContext::Global,        "Ctrl+Z" },
        { "edit.redo",                 "Redo",                ActionContext::Global,        "Ctrl+Y" },
        { "edit.redoAlt",              "Redo (alternate)",    ActionContext::Global,        "Ctrl+Shift+Z" },
        { "file.newScene",             "New Scene",           ActionContext::Global,        "Ctrl+N" },
        { "file.openScene",            "Open Scene",          ActionContext::Global,        "Ctrl+O" },
        { "file.saveScene",            "Save Scene",          ActionContext::Global,        "Ctrl+S" },
        { "edit.cut",                  "Cut",                 ActionContext::Global,        "Ctrl+X" },
        { "edit.copy",                 "Copy",                ActionContext::Global,        "Ctrl+C" },
        { "edit.paste",                "Paste",               ActionContext::Global,        "Ctrl+V" },
        { "edit.duplicate",            "Duplicate",           ActionContext::Global,        "Ctrl+D" },
        { "document.close",            "Close Document",      ActionContext::Global,        "Ctrl+W" },
        { "editor.view.perspective",   "Perspective View",    ActionContext::Global,        "Alt+G" },
        { "editor.view.ortho2D",       "2D View",             ActionContext::Global,        "Alt+J" },
        { "editor.view.frameSelected", "Frame Selection",     ActionContext::Global,        "F" },
        { "editor.view.frameAll",      "Frame All",           ActionContext::Global,        "Home" },
        // Viewport (needs viewport focus; physical: they are a position cluster)
        { "editor.viewport.toolSelect",    "Select Tool",     ActionContext::Viewport,      "[KeyQ]" },
        { "editor.viewport.toolTranslate", "Move Tool",       ActionContext::Viewport,      "[KeyW]" },
        { "editor.viewport.toolRotate",    "Rotate Tool",     ActionContext::Viewport,      "[KeyE]" },
        { "editor.viewport.toolScale",     "Scale Tool",      ActionContext::Viewport,      "[KeyR]" },
        { "editor.camera.flyForward",  "Fly Forward",         ActionContext::Viewport,      "[KeyW]" },
        { "editor.camera.flyBack",     "Fly Back",            ActionContext::Viewport,      "[KeyS]" },
        { "editor.camera.flyLeft",     "Fly Left",            ActionContext::Viewport,      "[KeyA]" },
        { "editor.camera.flyRight",    "Fly Right",           ActionContext::Viewport,      "[KeyD]" },
        { "editor.camera.flyUp",       "Fly Up",              ActionContext::Viewport,      "[KeyE]" },
        { "editor.camera.flyDown",     "Fly Down",            ActionContext::Viewport,      "[KeyQ]" },
        // Documents and panels (the ImGui route)
        { "document.save",             "Save Document",       ActionContext::Document,      "Ctrl+S" },
        { "outliner.rename",           "Rename Entity",       ActionContext::Outliner,      "F2" },
        { "outliner.delete",           "Delete Entity",       ActionContext::Outliner,      "Delete" },
        { "assets.selectPrev",         "Previous Asset",      ActionContext::AssetBrowser,  "Up" },
        { "assets.selectNext",         "Next Asset",          ActionContext::AssetBrowser,  "Down" },
        { "assets.open",               "Open Asset",          ActionContext::AssetBrowser,  "Enter" },
        { "assets.rename",             "Rename Asset",        ActionContext::AssetBrowser,  "F2" },
        { "assets.duplicate",          "Duplicate Asset",     ActionContext::AssetBrowser,  "Ctrl+D" },
        { "assets.delete",             "Delete Asset",        ActionContext::AssetBrowser,  "Delete" },
        { "graph.frameSelected",       "Frame Selection (graph)", ActionContext::Graph,     "F" },
        { "graph.copy",                "Copy Nodes",          ActionContext::Graph,         "Ctrl+C" },
        { "graph.cut",                 "Cut Nodes",           ActionContext::Graph,         "Ctrl+X" },
        { "graph.paste",               "Paste Nodes",         ActionContext::Graph,         "Ctrl+V" },
        { "graph.duplicate",           "Duplicate Nodes",     ActionContext::Graph,         "Ctrl+D" },
        { "graph.delete",              "Delete Nodes",        ActionContext::Graph,         "Delete" },
        { "console.copy",              "Copy Log Rows",       ActionContext::Console,       "Ctrl+C" },
        { "input.selectPrev",          "Previous Row",        ActionContext::InputDocument, "Up" },
        { "input.selectNext",          "Next Row",            ActionContext::InputDocument, "Down" },
        { "input.collapseOrParent",    "Collapse / Parent",   ActionContext::InputDocument, "Left" },
        { "input.expandOrChild",       "Expand / First Child",ActionContext::InputDocument, "Right" },
        { "input.rebind",              "Rebind",              ActionContext::InputDocument, "Enter" },
        { "input.rename",              "Rename",              ActionContext::InputDocument, "F2" },
        { "input.delete",              "Delete",              ActionContext::InputDocument, "Delete" },
        // Text: whatever owns the keyboard right now (a field, a drag, a modal)
        { "ui.cancel",                 "Cancel (field, drag, dialog)", ActionContext::Text, "Escape" },
        { "ui.confirm",                "Confirm (field, dialog)",      ActionContext::Text, "Enter" },
    } };
}
