// editor.undo.* (spec 2026-09-30 s2.4; settings S6-33): the EditorUndoSettings
// struct, read into the UndoLimits the editor pushes into its CommandStack.
#include <catch2/catch_test_macros.hpp>

#include "Helpers/SettingsSweep.hpp"
#include "App/UndoSettings.hpp"

TEST_CASE("editor.undo.* is EditorUndoSettings and feeds UndoLimits, clamped to its ranges", "[editor][undo][cvar][sweep]")
{
    using namespace Arcane;
    const Editor::EditorUndoSettings d{};
    CHECK(d.maxSteps == 100); CHECK(d.byteBudgetMB == 512); CHECK(d.spillThresholdKB == 256);
    Test::RequireDefault("editor.undo.maxSteps", CVarValue::Int32(100));
    const UndoLimits l = Editor::ReadUndoLimits();
    CHECK(l.maxSteps == 100); CHECK(l.byteBudget == 512ull << 20); CHECK(l.spillThreshold == 256ull << 10);
    CVarRegistry& reg = CVarRegistry::Get();
    reg.Set(reg.Find("editor.undo.byteBudgetMB"), CVarValue::Int32(1), SetBy::Console);   // below the 16 MB floor
    reg.PublishImmediate();
    CHECK(Editor::ReadUndoLimits().byteBudget == 16ull << 20);
    reg.RevertLayer(SetBy::Console); reg.PublishImmediate();
}
