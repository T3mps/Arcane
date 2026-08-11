// Edge: per-key rising/falling edge tracking (architecture pass sec 6).
#include <catch2/catch_test_macros.hpp>
#include "App/InputEdges.hpp"

using Arcane::Editor::Edge;

TEST_CASE("Edge reports rising and falling once each", "[editor][input]")
{
    Edge e;
    e.Update(true);
    CHECK(e.pressed); CHECK(e.down); CHECK_FALSE(e.released);
    e.Update(true);                     // held: no repeat
    CHECK_FALSE(e.pressed); CHECK(e.down);
    e.Update(false);
    CHECK(e.released); CHECK_FALSE(e.down); CHECK_FALSE(e.pressed);
    e.Update(false);
    CHECK_FALSE(e.released);
}

TEST_CASE("a skipped Update means a dead key, not auto-repeat", "[editor][input]")
{
    // The designed failure mode: forgetting the per-frame Update leaves
    // pressed stale-false after the first frame -- the key goes dead instead
    // of firing every frame (what a forgotten write-back used to cause).
    Edge e;
    e.Update(true);
    CHECK(e.pressed);
    // no Update this frame -- a consumer re-reading sees the OLD edge only once
    e.Update(true);
    CHECK_FALSE(e.pressed);
}
