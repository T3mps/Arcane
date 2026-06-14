// Physics M6 P1.7: TileGrid static broadphase -- the keystone.
//
// PORT + MODERNIZE of Client/src/physics/TileGrid.lua (see TileGrid.hpp). The
// Lua stores nothing for walls and answers queries straight from cell flags,
// merging a per-row run of consecutive solid cells into ONE virtual fixture
// (the greedy rectangle merge -- it kills the tile-seam internal-edge catch).
// We port that ALGORITHM; we REFORMULATE the cell->world coordinate math from
// iso (the Lua's diamonds/parallelograms) to plain CARTESIAN axis-aligned
// rects. Because iso is dropped there is NO Lua bit-match for the rect coords;
// the oracle is a SELF-DEFINED Cartesian grid with analytically-derived merged
// rects, which is correct + expected for this task.
//
// Two test groups:
//   1) MERGED-RECT SET -- the merge algorithm. Reformulates the harness
//      "== TileGrid ==" block (main.lua ~160-215) to Cartesian: an 8x4 grid
//      with a row-run + a two-span (gapped) row. Query the whole grid -> the
//      correct merged rects, asserted ANALYTICALLY from cellSize/origin + the
//      per-row-merge rule. Single run -> 1 rect; gap -> 2 rects; offscreen ->
//      0; partial overlap covered.
//   2) TILE-SEAM NO-CATCH -- the keystone property. A swept capsule glides
//      ACROSS a merged horizontal span with no internal-edge catch: because
//      consecutive solid cells merge into ONE rect there are no internal
//      vertical edges between cells, so a capsule sweeping parallel just above
//      the span clears its full length instead of catching at a cell boundary.

#include <cmath>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <Arcane/Physics/Shapes.hpp>
#include <Arcane/Physics/Broadphase/Broadphase.hpp>
#include <Arcane/Physics/Broadphase/Passability.hpp>
#include <Arcane/Physics/Broadphase/TileGrid.hpp>
#include <Arcane/Physics/Narrowphase/Gjk.hpp>

using namespace Arcane::Physics;
using Catch::Approx;

namespace
{
    // Build the harness's 8x4 grid (reformulated to 0-based Cartesian).
    //
    // Lua (1-based) walls: (3,2)(4,2)(5,2) and (2,3)(6,3)(7,3).
    // 0-based: row 1 cols 2,3,4 (one merged run) and row 2 cols 1, 5, 6
    // (col 1 alone + a 5..6 run -> TWO spans). Total = 3 merged rects.
    constexpr int kW = 8;
    constexpr int kH = 4;

    GridPassability MakeHarnessGrid()
    {
        GridPassability g(kW, kH);
        // row 1 (0-based): a single merged run, cols 2..4
        g.SetSolid(2, 1, true);
        g.SetSolid(3, 1, true);
        g.SetSolid(4, 1, true);
        // row 2 (0-based): a gapped row -> two runs, col 1 and cols 5..6
        g.SetSolid(1, 2, true);
        g.SetSolid(5, 2, true);
        g.SetSolid(6, 2, true);
        return g;
    }

    // Find a rect in `rects` whose min/max match (within f32 tolerance).
    bool HasRect(const std::vector<Aabb2>& rects, Real minx, Real miny,
                 Real maxx, Real maxy)
    {
        for (const Aabb2& r : rects)
        {
            if (r.min.x == Approx(minx) && r.min.y == Approx(miny) &&
                r.max.x == Approx(maxx) && r.max.y == Approx(maxy))
            {
                return true;
            }
        }
        return false;
    }
} // namespace

// ====================================================================
// 1) Merged-rect set: the greedy per-row run-merge, analytic Cartesian oracle.
// ====================================================================
TEST_CASE("physics: tilegrid per-row merge yields the analytic rect set",
          "[physics]")
{
    const Real     cs = Real(32);
    const Vec2     origin(Real(0), Real(0));
    GridPassability grid = MakeHarnessGrid();
    TileGrid       tg(grid, cs, origin);

    // Whole-grid query: the world box is exactly the 8x4 grid extent. With
    // origin {0,0} and cellSize 32 that is [0,0]..[256,128]. PAD adds slack but
    // the scan still clamps to the grid bounds.
    const Aabb2 whole{ Vec2(Real(0), Real(0)), Vec2(Real(kW) * cs, Real(kH) * cs) };
    std::vector<Aabb2> rects;
    const int n = tg.Query(whole, rects);

    // THREE merged spans total (harness: eq(n, 3, "three merged spans total")).
    CHECK(n == 3);
    CHECK(rects.size() == 3u);

    // Analytic expectations (cell (cx,cy) -> [cx*cs, cy*cs]..[(cx+1)*cs,(cy+1)*cs];
    // a run runStart..runEnd in row cy -> [runStart*cs, cy*cs]..[(runEnd+1)*cs,(cy+1)*cs]):
    //
    //   row 1 run 2..4 -> [2*32, 1*32]..[5*32, 2*32] = [64,32]..[160,64]
    //   row 2 run 1..1 -> [1*32, 2*32]..[2*32, 3*32] = [32,64]..[ 64,96]
    //   row 2 run 5..6 -> [5*32, 2*32]..[7*32, 3*32] = [160,64]..[224,96]
    CHECK(HasRect(rects, 64, 32, 160, 64));  // the single merged run
    CHECK(HasRect(rects, 32, 64, 64, 96));   // gapped row, first run (col 1)
    CHECK(HasRect(rects, 160, 64, 224, 96)); // gapped row, second run (5..6)

    // The merged run spans EXACTLY 3 cells wide (96 px) -- one rect, not three.
    // (Per-cell rects would be 32 px each with internal seams; the merge fuses
    // them into a single 96 px rect with no internal vertical edges.)
    for (const Aabb2& r : rects)
    {
        if (r.min.x == Approx(64) && r.min.y == Approx(32))
        {
            CHECK((r.max.x - r.min.x) == Approx(Real(3) * cs));
            CHECK((r.max.y - r.min.y) == Approx(cs));
        }
    }
}

TEST_CASE("physics: tilegrid offscreen query is empty, partial overlap finds runs",
          "[physics]")
{
    const Real     cs = Real(32);
    GridPassability grid = MakeHarnessGrid();
    TileGrid       tg(grid, cs, Vec2(Real(0), Real(0)));

    std::vector<Aabb2> rects;

    // Far offscreen query (the harness's offscreen-empty check): no in-bounds
    // solid cell -> 0 rects.
    const Aabb2 offscreen{ Vec2(Real(-10000), Real(-10000)),
                           Vec2(Real(-9000), Real(-9000)) };
    CHECK(tg.Query(offscreen, rects) == 0);
    CHECK(rects.empty());

    // A query that only PARTIALLY overlaps the grid: a tight window over the
    // gapped row's second run only (cells 5..6 of row 2 = world [160,64]..[224,96]).
    // PAD = 1 widens the cell scan, so it can also pick up the row-1 run above
    // (cells 2..4 of row 1) and col 1 of row 2 -- assert the 5..6 run is found
    // and the result is a subset of the full 3 (not all the offscreen junk).
    const Aabb2 window{ Vec2(Real(170), Real(70)), Vec2(Real(200), Real(90)) };
    const int   n = tg.Query(window, rects);
    CHECK(n >= 1);
    CHECK(static_cast<std::size_t>(n) == rects.size());
    CHECK(HasRect(rects, 160, 64, 224, 96)); // the 5..6 run is present

    // An empty grid (no solids) returns 0 for any query.
    GridPassability emptyGrid(kW, kH);
    TileGrid        emptyTg(emptyGrid, cs);
    const Aabb2     whole{ Vec2(Real(0), Real(0)),
                       Vec2(Real(kW) * cs, Real(kH) * cs) };
    CHECK(emptyTg.Query(whole, rects) == 0);
    CHECK(rects.empty());
}

TEST_CASE("physics: tilegrid honors a non-zero origin", "[physics]")
{
    // The Cartesian cell->world mapping offsets by `origin`; verify the run
    // rect shifts by exactly the origin.
    const Real      cs = Real(32);
    const Vec2      origin(Real(1000), Real(-500));
    GridPassability grid = MakeHarnessGrid();
    TileGrid        tg(grid, cs, origin);

    const Aabb2        whole{ origin, origin + Vec2(Real(kW) * cs, Real(kH) * cs) };
    std::vector<Aabb2> rects;
    CHECK(tg.Query(whole, rects) == 3);

    // row 1 run 2..4 shifted by origin: [1000+64, -500+32]..[1000+160, -500+64]
    CHECK(HasRect(rects, Real(1064), Real(-468), Real(1160), Real(-436)));
}

TEST_CASE("physics: tilegrid Query reuses the out vector (zero steady-state alloc)",
          "[physics]")
{
    const Real      cs = Real(32);
    GridPassability grid = MakeHarnessGrid();
    TileGrid        tg(grid, cs);

    const Aabb2 whole{ Vec2(Real(0), Real(0)),
                       Vec2(Real(kW) * cs, Real(kH) * cs) };
    std::vector<Aabb2> out;

    tg.Query(whole, out);
    const std::size_t cap0 = out.capacity();
    REQUIRE(out.size() == 3u);
    REQUIRE(cap0 >= 3u);

    // Repeated queries must NOT grow the capacity (clear() + push_back reuse).
    for (int i = 0; i < 200; ++i)
    {
        tg.Query(whole, out);
    }
    CHECK(out.capacity() == cap0);
    CHECK(out.size() == 3u);

    // An offscreen query clears but keeps the capacity (no shrink, no grow).
    tg.Query(Aabb2{ Vec2(Real(-1e6f), Real(-1e6f)), Vec2(Real(-9e5f), Real(-9e5f)) },
             out);
    CHECK(out.empty());
    CHECK(out.capacity() == cap0);
}

// ====================================================================
// 2) Tile-seam no-catch: the keystone property.
//
// A horizontal run of N solid cells merges into ONE rect, so there are NO
// internal vertical edges between the cells. A capsule swept HORIZONTALLY just
// above the span's top edge therefore travels its full length with no spurious
// early stop at an internal cell boundary -- it either clears the span
// entirely, or (if its path dips into the span) stops only at the span's OUTER
// extent. Contrast: per-cell (UN-merged) rects would expose a vertical edge at
// every cell boundary, and a grazing sweep could catch on one of them. The
// merge eliminates them. (The full CharacterController slide-down-a-column test
// is P1.10; here we prove the per-row merge removes internal horizontal-run edges.)
// ====================================================================
TEST_CASE("physics: tilegrid merged span has no internal seam to catch on",
          "[physics]")
{
    const Real cs = Real(32);
    // A single long horizontal run: row 0, cols 0..4 (5 cells). origin {0,0}.
    // Merged rect = [0,0]..[160,32]. Internal cell boundaries WOULD be at
    // x = 32, 64, 96, 128 -- the merge fuses them away.
    GridPassability grid(/*w*/ 8, /*h*/ 2);
    for (int cx = 0; cx <= 4; ++cx)
    {
        grid.SetSolid(cx, 0, true);
    }
    TileGrid tg(grid, cs);

    const Aabb2 whole{ Vec2(Real(0), Real(0)), Vec2(Real(8) * cs, Real(2) * cs) };
    std::vector<Aabb2> rects;
    const int          n = tg.Query(whole, rects);

    // EXACTLY ONE merged rect for the 5-cell run -- the seam-killer. If the
    // grid emitted per-cell rects we'd see 5 here (and the internal edges).
    REQUIRE(n == 1);
    const Aabb2 span = rects[0];
    REQUIRE(span.min.x == Approx(0));
    REQUIRE(span.min.y == Approx(0));
    REQUIRE(span.max.x == Approx(Real(5) * cs)); // 160 -- the OUTER extent
    REQUIRE(span.max.y == Approx(cs));           // 32

    // Build the merged rect as a collidable polygon (CCW box) for the cast.
    const Aabb2 r        = span;
    const Vec2  poly[4]  = { Vec2(r.min.x, r.min.y), Vec2(r.max.x, r.min.y),
                             Vec2(r.max.x, r.max.y), Vec2(r.min.x, r.max.y) };

    // A capsule grazing JUST ABOVE the span's top edge (y = 32). Capsule
    // half-length 6, radius 4; center placed so its bottom (y_center + radius)
    // sits a hair above the span top -> it should NOT touch the span at all,
    // and crucially must not catch at any internal x (there are none).
    const Shape cap = MakeCapsule(/*halfLen*/ Real(6), /*radius*/ Real(4));

    // Start to the LEFT of the span, sweep RIGHT across its full width, riding
    // at y just above the top edge: center y = top - radius - gap.
    const Real gap     = Real(1.0);
    const Real rideY   = span.min.y - Real(4) - gap; // above the top edge
    const Vec2 start(span.min.x - Real(20), rideY);
    const Vec2 endPt(span.max.x + Real(20), rideY);
    const Vec2 translation = endPt - start;

    Transform   xf;
    xf.position = start;
    const ShapeCastResult clearCast =
        ShapeCastPoly(cap, xf, translation, poly, 4);

    // Riding above the top edge with a clearance gap: the swept capsule never
    // reaches the span (no internal edge to catch on along the way) -> no hit,
    // it travels the full translation.
    CHECK_FALSE(clearCast.hit);

    // Now drive the capsule THROUGH the span's left face at mid-height: it must
    // stop at the span's OUTER left extent, NOT at an internal cell boundary
    // (x = 32/64/96/128 do not exist as edges anymore). Sweep left->right at the
    // span's vertical mid-line.
    const Real  midY = (span.min.y + span.max.y) * Real(0.5);
    Transform   xf2;
    xf2.position = Vec2(span.min.x - Real(40), midY);
    const Vec2  trans2 = Vec2(Real(80), Real(0)); // drive into the left face
    const ShapeCastResult faceCast =
        ShapeCastPoly(cap, xf2, trans2, poly, 4);

    REQUIRE(faceCast.hit);
    // Impact center x at TOI: start.x + trans2.x * t. It must land at the OUTER
    // left face (x = span.min.x), offset by the capsule's right extent
    // (halfLen + radius = 10) and the cast tolerance. The KEY assertion: the
    // stop is at the span's outer extent, far from any internal cell boundary
    // (the first internal boundary would have been x = 32, well to the right of
    // the outer left face at x = 0).
    const Real impactX = xf2.position.x + trans2.x * faceCast.t;
    // The capsule's leading (right) edge at impact ~ impactX + (halfLen+radius).
    const Real leadingX = impactX + (Real(6) + Real(4));
    // Leading edge stops at (or a tolerance short of) the OUTER left face x = 0.
    CHECK(leadingX <= Approx(span.min.x).margin(0.2));
    // And it is NOWHERE NEAR the first internal cell boundary x = 32 (which the
    // merge eliminated) -- it stopped at the outer extent, not mid-run.
    CHECK(leadingX < Real(32) - Real(10));
}
