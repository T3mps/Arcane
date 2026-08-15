// DeferredPick -- the graph arm's viewport click-pick, which cannot answer in
// the frame it is asked (NRI Phase 3, Task 9).
//
// WHY THIS FILE IS WORTH ITS WEIGHT. EditorApp*.cpp is not compiled into
// ArcaneTests (see its own header: a green suite proves nothing about frame
// order), so the phases that DRIVE this machine -- 10's TakeRequest and 17's
// Land/Arm -- have no headless coverage at all and never will. The machine
// itself is the part that decides whether a click changes the RIGHT thing, and
// it is pure: no ImGui, no device, no registry, entities as opaque handles. So
// it is extracted and driven directly, including the orderings a GPU run would
// take frames to reach and a desk session might never reach at all (a second
// click landing mid-flight; a scene replaced between the click and its answer).
//
// What it deliberately does NOT cover, because they are not in this object:
// whether a click was legal (the four guards at EditorApp::HandleViewportPick)
// and what a resolved entity means (Select / Toggle / Clear).
#include <catch2/catch_test_macros.hpp>

#include "Viewport/DeferredPick.hpp"

#include <Astra/Entity/Entity.hpp>

#include <vector>

using Arcane::Editor::DeferredPick;

namespace
{
    // The id<->entity table a frame's CollectPickables would produce: the k-th
    // entity IS hit-proxy id k+1 (PickEmit.hpp's ordering contract).
    std::vector<Astra::Entity> Table(std::initializer_list<std::uint32_t> ids)
    {
        std::vector<Astra::Entity> out;
        for (const std::uint32_t id : ids)
            out.push_back(Astra::Entity(id, 0));
        return out;
    }
}

TEST_CASE("deferred pick: a click resolves through the table of the frame that rasterised it",
          "[editor]")
{
    DeferredPick pick;
    CHECK_FALSE(pick.Busy());
    CHECK(pick.State() == DeferredPick::Phase::Idle);

    // Phase 17: the click. Nothing has been minted yet -- the ticket belongs to
    // the frame that actually probes, not to the click.
    pick.Arm(glm::ivec2(40, 12), /*ctrlHeld=*/false, /*sceneEpoch=*/7, /*playMode=*/false);
    REQUIRE(pick.State() == DeferredPick::Phase::Armed);
    CHECK(pick.Busy());
    CHECK(pick.Ticket() == 0u);

    // Phase 10 of the NEXT frame: the request, with THIS frame's table.
    const std::vector<Astra::Entity> table = Table({ 11, 22, 33 });
    const auto request = pick.TakeRequest(table);
    REQUIRE(request.has_value());
    CHECK(request->pixel == glm::ivec2(40, 12));
    CHECK(request->ticket != 0u);            // 0 is the "unlabelled" default and must never be minted
    CHECK(pick.State() == DeferredPick::Phase::InFlight);
    CHECK(pick.Ticket() == request->ticket);

    // A second TakeRequest without a new click is a no-op: the chain declares a
    // readback every frame it is on, and every one of those must not re-mint.
    CHECK_FALSE(pick.TakeRequest(table).has_value());
    CHECK(pick.Ticket() == request->ticket);

    // Phase 17, two frames later: id 2 is the SECOND entity of the table above.
    const auto hit = pick.Land(request->ticket, /*id=*/2u, /*sceneEpoch=*/7, /*playMode=*/false);
    REQUIRE(hit.has_value());
    CHECK(hit->entity == table[1]);
    CHECK_FALSE(hit->ctrlHeld);

    // Consumed exactly once -- ProbeResult keeps reporting the same pair every
    // frame until the next drain replaces it, so this is the ONLY thing
    // standing between one click and a selection that toggles forever.
    CHECK(pick.State() == DeferredPick::Phase::Idle);
    CHECK_FALSE(pick.Busy());
    CHECK_FALSE(pick.Land(request->ticket, 2u, 7, false).has_value());
}

TEST_CASE("deferred pick: id 0 is BACKGROUND, a real answer rather than a failure", "[editor]")
{
    // The NVRHI arm clears the selection on an empty-space click and treats
    // ctrl+click on empty space as a miss instead. Both need the id-0 landing
    // to be a HIT with an invalid entity, not a nullopt -- nullopt means "do
    // not touch the selection", which is the opposite instruction.
    DeferredPick pick;
    pick.Arm(glm::ivec2(0, 0), /*ctrlHeld=*/true, 1, false);
    const auto request = pick.TakeRequest(Table({ 5 }));
    REQUIRE(request.has_value());

    const auto hit = pick.Land(request->ticket, /*id=*/0u, 1, false);
    REQUIRE(hit.has_value());
    CHECK_FALSE(hit->entity.IsValid());
    // ...and the ctrl state is the one captured AT THE CLICK, which is what
    // decides between "miss" and "deselect all" frames later.
    CHECK(hit->ctrlHeld);
}

TEST_CASE("deferred pick: an out-of-range id is background, not a read past the table", "[editor]")
{
    DeferredPick pick;
    pick.Arm(glm::ivec2(1, 1), false, 1, false);
    const auto request = pick.TakeRequest(Table({ 5, 6 }));
    REQUIRE(request.has_value());

    // The id pass can only ever emit 1..N for an N-entry table -- but the value
    // arrives through mapped GPU memory two frames after the table was taken,
    // and PickEntityForId's contract is that an out-of-range k is background.
    const auto hit = pick.Land(request->ticket, /*id=*/9u, 1, false);
    REQUIRE(hit.has_value());
    CHECK_FALSE(hit->entity.IsValid());
}

TEST_CASE("deferred pick: only the outstanding ticket's answer applies", "[editor]")
{
    // The editor's probe pixel moves, and several clicks can be in flight
    // across a slow frame. Without the ticket, the value in hand is
    // indistinguishable from the answer to a click the user has already
    // replaced -- i.e. a selection change they did not ask for.
    DeferredPick pick;

    pick.Arm(glm::ivec2(10, 10), false, 1, false);
    const auto first = pick.TakeRequest(Table({ 100, 200 }));
    REQUIRE(first.has_value());

    // The user clicks again before the first answer lands. Newest wins.
    pick.Arm(glm::ivec2(20, 20), false, 1, false);
    CHECK(pick.State() == DeferredPick::Phase::Armed);
    const auto second = pick.TakeRequest(Table({ 300, 400 }));
    REQUIRE(second.has_value());
    CHECK(second->ticket != first->ticket);
    CHECK(second->pixel == glm::ivec2(20, 20));

    // The FIRST click's copy lands first -- it was recorded first. Dropped.
    CHECK_FALSE(pick.Land(first->ticket, 1u, 1, false).has_value());
    CHECK(pick.State() == DeferredPick::Phase::InFlight);   // still waiting for its own

    // A ticket of 0 -- what an idle frame's readback carries -- never applies.
    CHECK_FALSE(pick.Land(0u, 1u, 1, false).has_value());

    // ...and the second one resolves through the SECOND table.
    const auto hit = pick.Land(second->ticket, 1u, 1, false);
    REQUIRE(hit.has_value());
    CHECK(hit->entity == Astra::Entity(300, 0));
}

TEST_CASE("deferred pick: a scene replaced between the click and its answer discards it",
          "[editor]")
{
    // The retained table names entities of the scene the user CLICKED on. If
    // New/Open Scene, a project switch, or a Play start/stop has swapped the
    // registry since, those handles mean nothing -- and Astra recycles entity
    // slots, so a stale handle can name a DIFFERENT live entity rather than
    // failing to resolve. Changing nothing is the safe direction.
    SECTION("the scene epoch moved")
    {
        DeferredPick pick;
        pick.Arm(glm::ivec2(3, 4), false, /*sceneEpoch=*/1, false);
        const auto request = pick.TakeRequest(Table({ 77 }));
        REQUIRE(request.has_value());

        CHECK_FALSE(pick.Land(request->ticket, 1u, /*sceneEpoch=*/2, false).has_value());
        // CONSUMED anyway: a stale answer to OUR ticket is still the answer to
        // it, and leaving the request outstanding would keep the whole pick +
        // outline chain declared for the rest of the session.
        CHECK(pick.State() == DeferredPick::Phase::Idle);
    }

    SECTION("play mode flipped")
    {
        DeferredPick pick;
        pick.Arm(glm::ivec2(3, 4), false, 1, /*playMode=*/false);
        const auto request = pick.TakeRequest(Table({ 77 }));
        REQUIRE(request.has_value());

        CHECK_FALSE(pick.Land(request->ticket, 1u, 1, /*playMode=*/true).has_value());
        CHECK(pick.State() == DeferredPick::Phase::Idle);
    }

    SECTION("both unchanged -- the control, so the sections above pin the GUARD and not the plumbing")
    {
        DeferredPick pick;
        pick.Arm(glm::ivec2(3, 4), false, 1, false);
        const auto request = pick.TakeRequest(Table({ 77 }));
        REQUIRE(request.has_value());
        REQUIRE(pick.Land(request->ticket, 1u, 1, false).has_value());
    }
}

TEST_CASE("deferred pick: Reset drops an outstanding request without applying it", "[editor]")
{
    // EditorApp::NoteSceneReplaced calls this, so the epoch check above is a
    // backstop rather than the only line of defence: a scene swap drops the
    // request immediately instead of waiting for its answer to arrive and be
    // rejected -- which also releases the pick chain a frame or two sooner.
    DeferredPick pick;
    pick.Arm(glm::ivec2(3, 4), false, 1, false);
    const auto request = pick.TakeRequest(Table({ 77 }));
    REQUIRE(request.has_value());

    pick.Reset();
    CHECK_FALSE(pick.Busy());
    CHECK_FALSE(pick.Land(request->ticket, 1u, 1, false).has_value());

    // ...and the machine is reusable afterwards, with a ticket that cannot
    // collide with the dropped one.
    pick.Arm(glm::ivec2(5, 6), false, 1, false);
    const auto next = pick.TakeRequest(Table({ 88 }));
    REQUIRE(next.has_value());
    CHECK(next->ticket != request->ticket);
}

TEST_CASE("deferred pick: a request that never lands is abandoned, once and loudly", "[editor]")
{
    // The readback IS guaranteed to drain after kSwapchainFramesInFlight
    // rendered frames, and the host keeps the chain declared for exactly that
    // long -- so this should never fire. It exists because the failure it
    // guards is a machine that never returns to Idle, which would silently keep
    // an outline chain declared for the rest of the session.
    DeferredPick pick;
    CHECK_FALSE(pick.TickAndMaybeAbandon());   // nothing outstanding: no tick, no give-up

    pick.Arm(glm::ivec2(1, 2), false, 1, false);
    // Armed but not yet in flight: the budget counts FRAMES IN FLIGHT, and a
    // click waiting for its declaration frame has not spent one.
    CHECK_FALSE(pick.TickAndMaybeAbandon());
    CHECK(pick.State() == DeferredPick::Phase::Armed);

    const auto request = pick.TakeRequest(Table({ 9 }));
    REQUIRE(request.has_value());

    for (std::uint32_t frame = 0; frame < DeferredPick::kMaxFramesInFlight; ++frame)
    {
        INFO("frame " << frame);
        REQUIRE_FALSE(pick.TickAndMaybeAbandon());
        REQUIRE(pick.Busy());
    }
    // EXACTLY ON the frame it gives up, so the caller can say so once.
    CHECK(pick.TickAndMaybeAbandon());
    CHECK_FALSE(pick.Busy());
    CHECK_FALSE(pick.TickAndMaybeAbandon());

    // ...and the abandoned ticket can never resurface as a selection change.
    CHECK_FALSE(pick.Land(request->ticket, 1u, 1, false).has_value());
}

TEST_CASE("deferred pick: the retained table survives the live one being rebuilt", "[editor]")
{
    // The whole reason the table is COPIED at TakeRequest rather than borrowed:
    // the caller's vector is rebuilt by CollectPickables every frame, and the
    // answer arrives kSwapchainFramesInFlight frames later.
    DeferredPick pick;
    std::vector<Astra::Entity> live = Table({ 1, 2, 3 });

    pick.Arm(glm::ivec2(0, 0), false, 1, false);
    const auto request = pick.TakeRequest(live);
    REQUIRE(request.has_value());

    // The host's vector is cleared and refilled with a completely different
    // scene's silhouettes, twice, exactly as the frames in between would.
    live = Table({ 40, 50 });
    live = Table({ 60 });

    const auto hit = pick.Land(request->ticket, /*id=*/3u, 1, false);
    REQUIRE(hit.has_value());
    CHECK(hit->entity == Astra::Entity(3, 0));   // the THIRD entity of the ORIGINAL table
}
