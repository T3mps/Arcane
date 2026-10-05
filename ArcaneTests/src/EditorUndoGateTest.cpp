// The one Play barrier + the Edit menu's undo state (spec 2026-09-30 s3.3b/d) -- pure.
#include <catch2/catch_test_macros.hpp>

#include "Scene/UndoGate.hpp"

#include <Astra/Registry/Registry.hpp>

using namespace Arcane::Editor;

TEST_CASE("UndoBarred is the Play barrier, and the document resolver obeys it", "[editor][undo]")
{
    STATIC_REQUIRE(UndoBarred(true));
    STATIC_REQUIRE_FALSE(UndoBarred(false));
    Astra::Registry registry;
    Arcane::CommandStack stack{[&registry]() -> Astra::Registry& { return registry; }};
    CHECK(ResolveDocumentUndo(false, &stack) == &stack);
    CHECK(ResolveDocumentUndo(true, &stack) == nullptr);
}

TEST_CASE("SceneConsumesUndoKeys yields to Play, an open transaction, and a focused settings window", "[editor][undo]")
{
    STATIC_REQUIRE(SceneConsumesUndoKeys(false, false, false));
    STATIC_REQUIRE_FALSE(SceneConsumesUndoKeys(true, false, false));    // Play
    STATIC_REQUIRE_FALSE(SceneConsumesUndoKeys(false, true, false));    // open transaction
    STATIC_REQUIRE_FALSE(SceneConsumesUndoKeys(false, false, true));    // settings focused
    STATIC_REQUIRE_FALSE(SceneConsumesUndoKeys(true, true, true));      // every bar at once
}

TEST_CASE("UndoMenuState: enabled with the step label in Edit mode", "[editor][undo]")
{
    const UndoMenuItem u = UndoMenuState(true, false, false, "", "Move");
    CHECK(u.enabled);
    CHECK(u.label == "Undo Move");
    CHECK(u.tooltip.empty());
    const UndoMenuItem r = UndoMenuState(false, false, false, "", "", /*redo*/ true);
    CHECK_FALSE(r.enabled);
    CHECK(r.label == "Redo");
}

TEST_CASE("UndoMenuState: disabled with its reason in Play and during an open edit", "[editor][undo]")
{
    const UndoMenuItem play = UndoMenuState(true, true, false, "", "Move");
    CHECK_FALSE(play.enabled);
    CHECK(play.tooltip == "Stop play mode to undo");
    CHECK(UndoMenuState(true, true, false, "", "Move", true).tooltip == "Stop play mode to redo");
    const UndoMenuItem txn = UndoMenuState(true, false, true, "", "Move");
    CHECK_FALSE(txn.enabled);
    CHECK(txn.tooltip == "Finish the current edit first");
}

TEST_CASE("UndoMenuState: \"Can't undo after: <reason>\" after a clear, until a step exists", "[editor][undo]")
{
    CHECK(UndoMenuState(false, false, false, "Opened scene level_one").label
          == "Can't undo after: Opened scene level_one");
    CHECK(UndoMenuState(true, false, false, "Opened scene level_one", "Move").label == "Undo Move");
    CHECK(UndoMenuState(false, false, false, "Switched project", "", true).label == "Redo");   // Undo only
    CHECK(UndoMenuState(false, false, false, "").label == "Undo");
}
