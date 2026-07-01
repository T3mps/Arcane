// Physics Task B: restore the "island is uniformly awake" invariant.
//
// A body that settles + sleeps resting PURELY on tile spans (no static anchor
// BODY) becomes a singleton SLEEPING island. When a still-awake neighbour later
// drifts into speculative contact, the pending-merge tail grafts the two islands
// together. Historically MergeIslands preserved awake flags verbatim, so an
// already-sleeping singleton was grafted into an AWAKE island WITHOUT being
// woken (the WakeMoverPair `moverIsMoving` gate declined to wake a near-idle
// incoming body). That left a MIXED island -- an awake dynamic and a sleeping
// dynamic sharing one island -- and the very next EmitContactConstraints emitted
// a constraint for the awake side while the sleeping side of the same contact
// tripped the "no emitted constraint references a SLEEPING dynamic" assert
// (PhysicsWorld.cpp, EmitContactConstraints). In a Debug build (asserts live)
// that ABORTS the process.
//
// The fix wakes the sleeping island at merge time so the merged island is
// uniformly awake. This test drives the exact stressor the narrowphase-MT span
// suite deliberately sidestepped (it disabled sleep specifically to avoid this
// invariant): a dense pile raining into a solid tile BOWL with sleep ENABLED.
// It (1) never trips the assert / aborts, and (2) asserts the post-step
// invariant every step -- no island ever simultaneously holds an awake dynamic
// and a sleeping dynamic (the precondition that fed the bad constraint).
//
// PRESENTATION-FREE + C++23-clean.

#include <cstdint>
#include <unordered_map>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <Arcane/Physics/PhysicsTypes.hpp>
#include <Arcane/Physics/Shapes.hpp>
#include <Arcane/Physics/PhysicsWorld.hpp>
#include <Arcane/Physics/Broadphase/Passability.hpp> // GridPassability (tile-span bowl)

using namespace Arcane::Physics;

namespace
{
    constexpr Real kStep = Real(1) / Real(60);

    // Tile-span bowl geometry -- mirrors PhysicsNarrowphaseMtTest's RunCaptureSpans:
    // a 64x64 grid of 20px cells shaped into a solid bowl (thick full-width floor
    // rows 40..43 -> a merged span with top face y=800, plus solid side walls so
    // the pile cannot escape sideways off the floor edges). Every resting body
    // sits on the merged tile SPAN -- there is NO static anchor BODY, so a settled
    // body sleeps as a SINGLETON island (the exact precondition for the bug).
    constexpr int  kSpanGridW       = 64;
    constexpr int  kSpanGridH       = 64;
    constexpr Real kSpanCellSize    = Real(20);
    constexpr int  kSpanFloorRow    = 40;
    constexpr int  kSpanFloorRowEnd = 43;
    constexpr int  kSpanWallRowTop  = 24;
    constexpr Real kSpanFloorTop    = Real(kSpanFloorRow) * kSpanCellSize; // 800

    void BuildBowl(GridPassability& grid)
    {
        for (int cy = kSpanFloorRow; cy <= kSpanFloorRowEnd; ++cy)
        {
            for (int cx = 0; cx < kSpanGridW; ++cx)
            {
                grid.SetSolid(cx, cy, true);
            }
        }
        for (int cy = kSpanWallRowTop; cy <= kSpanFloorRowEnd; ++cy)
        {
            grid.SetSolid(0, cy, true);
            grid.SetSolid(1, cy, true);
            grid.SetSolid(kSpanGridW - 2, cy, true);
            grid.SetSolid(kSpanGridW - 1, cy, true);
        }
    }

    // Rain N mixed-shape dynamic bodies INSIDE the bowl, above the floor. Spawn x
    // in [120,1160] keeps every body clear of the side walls; spawn y in [520,720]
    // is well ABOVE the floor top (800) so they fall onto the merged tile span.
    std::vector<BodyHandle> BuildRain(PhysicsWorld& w, int n)
    {
        std::uint32_t seed = 0xC0FFEEu;
        auto rnd = [&](Real a, Real b) -> Real
        {
            seed = seed * 1664525u + 1013904223u;
            return a + (b - a) * Real((seed >> 8) & 0xFFFF) / Real(65535);
        };

        std::vector<BodyHandle> handles;
        handles.reserve(static_cast<std::size_t>(n));

        for (int j = 0; j < n; ++j)
        {
            BodyDef d;
            d.type     = BodyType::Dynamic;
            d.density  = Real(1);
            d.friction = Real(0.4);
            d.position = Vec2(rnd(Real(120), Real(1160)),
                              rnd(Real(520), Real(720)));

            if (j % 3 == 0)
            {
                d.shape = MakeCircle(rnd(Real(6), Real(11)));
            }
            else if (j % 3 == 1)
            {
                d.shape         = MakeAabb(Real(8), Real(8));
                d.fixedRotation = true;
            }
            else
            {
                d.shape = MakeCapsule(Real(10), Real(5));
            }

            handles.push_back(w.AddBody(d));
        }

        return handles;
    }

    // The invariant under test: no island simultaneously contains an awake
    // dynamic and a sleeping dynamic. Group every dynamic body by its island root
    // and flag any root that reports BOTH an awake and a sleeping member. Returns
    // true when the invariant holds (uniformly-awake islands).
    bool IslandsUniformlyAwake(const PhysicsWorld& w,
                               const std::vector<BodyHandle>& bodies)
    {
        // root -> (sawAwake, sawAsleep).
        std::unordered_map<std::uint32_t, std::pair<bool, bool>> byRoot;
        byRoot.reserve(bodies.size());
        for (const BodyHandle h : bodies)
        {
            const std::uint32_t root = w.IslandRootOf(h.index);
            auto& seen = byRoot[root];
            if (w.IsAwake(h)) { seen.first  = true; }
            else              { seen.second = true; }
        }
        for (const auto& [root, seen] : byRoot)
        {
            (void)root;
            if (seen.first && seen.second)
            {
                return false; // mixed awake/asleep island -> invariant violated
            }
        }
        return true;
    }
}

// ---------------------------------------------------------------------------
// Dense pile raining into a solid tile BOWL with sleep ENABLED. Bodies settle
// and sleep resting purely on the merged tile span (singleton sleeping
// islands), while later arrivals keep drifting into speculative contact and
// firing island merges. Pre-fix this trips the EmitContactConstraints
// no-sleeping-dynamic assert (aborts in Debug) once a sleeping singleton is
// grafted into an awake island. Post-fix the merge wakes the sleeping side, so:
//   (a) the sim runs to completion with NO assertion failure / UB, AND
//   (b) the uniform-awake invariant holds after EVERY step -- no island ever
//       holds both an awake and a sleeping dynamic.
// ---------------------------------------------------------------------------
TEST_CASE("PhysicsIslandWakeMerge: tile-span pile with sleep keeps islands uniformly awake",
          "[physics][island]")
{
    GridPassability grid(kSpanGridW, kSpanGridH);
    BuildBowl(grid);

    WorldDef wd;
    wd.gravityY     = Real(400);
    wd.passability  = &grid;
    wd.tileCellSize = kSpanCellSize;
    wd.tileOrigin   = Vec2(Real(0), Real(0));
    // Sleep ENABLED (default threshold 8 px/s). This is the whole point: bodies
    // resting purely on the tile span DO sleep and become singleton sleeping
    // islands -- the precondition the narrowphase-MT span suite disabled sleep to
    // avoid. Do NOT set sleepThreshold to 0.
    PhysicsWorld w(wd);

    const int kBodies = 140;
    const std::vector<BodyHandle> handles = BuildRain(w, kBodies);

    // Step long enough for the pile to fall, settle, sleep, and re-merge as late
    // arrivals drift into the sleepers. The invariant must hold after EVERY step
    // (a mixed island would already have tripped the Debug assert INSIDE Step()).
    bool sawAnySleep = false;
    for (int k = 0; k < 600; ++k)
    {
        w.Step(kStep);
        REQUIRE(IslandsUniformlyAwake(w, handles));

        for (const BodyHandle h : handles)
        {
            if (!w.IsAwake(h)) { sawAnySleep = true; break; }
        }
    }

    // The scenario is only meaningful if bodies actually slept on the span (the
    // bug's precondition). If nothing ever slept, the invariant check above is
    // vacuous and this guard fails loud rather than passing silently.
    REQUIRE(sawAnySleep);

    // Containment sanity (mirrors RunCaptureSpans): the bowl held the pile on the
    // merged span rather than tunnelling through it (smaller y is higher, +y is
    // down). Proves spans were generated + effective, so the sleepers really did
    // rest on tile spans (no static anchor body).
    bool sawBoundedY = false;
    for (const BodyHandle h : handles)
    {
        REQUIRE(w.Position(h).y < kSpanFloorTop + Real(80));
        sawBoundedY = true;
    }
    REQUIRE(sawBoundedY);
}
