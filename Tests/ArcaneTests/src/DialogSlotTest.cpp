// DialogSlot: one in-flight async file-dialog result, epoch-guarded so a
// result from a dead project or a superseded dialog is dropped, never applied.
#include <catch2/catch_test_macros.hpp>
#include "App/DialogSlot.hpp"
#include <string>

using Arcane::Editor::DialogSlot;

TEST_CASE("Arm/Stash/Take round-trips a payload", "[editor][dialog]")
{
    DialogSlot<std::string> slot;
    const auto epoch = slot.Arm();
    slot.Stash(epoch, "C:/scene.arcscene");
    const auto got = slot.Take();
    REQUIRE(got.has_value());
    CHECK(*got == "C:/scene.arcscene");
    CHECK_FALSE(slot.Take().has_value());   // Take empties
}

TEST_CASE("a Stash with a stale epoch after Clear is dropped", "[editor][dialog]")
{
    DialogSlot<std::string> slot;
    const auto epoch = slot.Arm();
    slot.Clear();                       // project switch
    slot.Stash(epoch, "dead-project-result");
    CHECK_FALSE(slot.Take().has_value());
}

TEST_CASE("re-Arm supersedes the first dialog (the A2 double-dialog case)", "[editor][dialog]")
{
    struct InstanceResult { std::string path; int parent; };
    DialogSlot<InstanceResult> slot;
    const auto first  = slot.Arm();
    const auto second = slot.Arm();
    slot.Stash(first,  { "first.arcmat",  1 });   // dropped: superseded
    slot.Stash(second, { "second.arcmat", 2 });   // lands, parent rides WITH path
    const auto got = slot.Take();
    REQUIRE(got.has_value());
    CHECK(got->path == "second.arcmat");
    CHECK(got->parent == 2);
}

TEST_CASE("Arm drops an unconsumed prior result (last-writer-wins)", "[editor][dialog]")
{
    DialogSlot<std::string> slot;
    slot.Stash(slot.Arm(), "unconsumed");
    (void)slot.Arm();                   // user opened the dialog again
    CHECK_FALSE(slot.Take().has_value());
}
