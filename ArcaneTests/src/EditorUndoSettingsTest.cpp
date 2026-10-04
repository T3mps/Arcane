// editor.undo.* (spec 2026-09-30 s2.4): declared with ARC_CVAR (settings S1-8) and
// read into the UndoLimits the editor pushes into its CommandStack.
#include <catch2/catch_test_macros.hpp>

#include "App/UndoSettings.hpp"

#include <Arcane/Config/CVarRegistry.hpp>

TEST_CASE("editor.undo.* cvars feed UndoLimits, clamped to their ranges", "[editor][undo][cvar]")
{
    Arcane::CVarRegistry& cvars = Arcane::CVarRegistry::Get();
    const Arcane::UndoLimits d = Arcane::Editor::ReadUndoLimits(cvars);
    CHECK(d.maxSteps == 100);
    CHECK(d.byteBudget == 512ull << 20);
    CHECK(d.spillThreshold == 256ull << 10);

    const Arcane::CVarHandle budget = cvars.Find("editor.undo.byteBudgetMB");
    REQUIRE_FALSE(budget.IsStale());
    cvars.Set(budget, Arcane::CVarValue::Int32(1), Arcane::SetBy::Console);   // below the 16 MB floor
    cvars.Publish();
    CHECK(Arcane::Editor::ReadUndoLimits(cvars).byteBudget == 16ull << 20);
    cvars.Set(budget, Arcane::CVarValue::Int32(512), Arcane::SetBy::Console);
    cvars.Publish();

    REQUIRE_FALSE(cvars.Find("editor.undo.maxSteps").IsStale());
    REQUIRE_FALSE(cvars.Find("editor.undo.spillThresholdKB").IsStale());
}
