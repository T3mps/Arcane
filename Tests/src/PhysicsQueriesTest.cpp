// Physics M6 P1.9: PhysicsWorld queries -- Raycast / LineOfSight / ShapeCast /
// OverlapShape / QueryAABB.
//
// PORT NOTE: a BEHAVIORAL port of the physics_harness raycast / shapeCast / LOS
// blocks (Client/src/tests/physics_harness/main.lua ~420-456, ~608-684) and the
// world query methods (Client/src/physics/PhysicsWorld.lua raycast 502-604,
// queryAABB 473-484, Cast.lua shapeCast/rayVsBody). The harness blocks are
// ISO-COUPLED (Iso.toCellF lattice, iso world coords), so we DO NOT bit-match
// their iso numbers. Instead we REFORMULATE to a SELF-DEFINED CARTESIAN oracle
// (TileGrid cellSize/origin + GridPassability cells) and assert the SAME
// behavioral / analytic invariants:
//   * Raycast vs cells: a wall cell in the path is hit with 0 < t < 1 and the
//     correct cellX/cellY; a clear column misses; a degenerate ray returns none.
//   * Raycast vs bodies: AABB slab hit (exact); a near body beats a farther
//     wall (nearest wins); a diagonal ray through a circle center hits at
//     EXACTLY r from the center (the harness "exact surface hit r=10" invariant).
//   * LineOfSight: a TALL cell blocks LOS; a LOW cell (BlocksSight=false) does
//     NOT; a clear column has LOS; degenerate is false.
//   * ShapeCast: a circle swept toward a wall stops a radius SHORT of where a
//     ray hits (cast.t < ray.t); a clear cast misses; the hit reports a near-
//     touching distance + a push-back normal.
//   * OverlapShape: a query shape returns exactly the overlapping bodies.
//   * QueryAABB: both body kinds (P1.8 coverage + a non-overlap exclusion).
//
// The cell DDA is the Cartesian (min-corner) reformulation of the Lua's
// centered iso lattice -- consistent with TileGrid::CellBox / IsSolid.
//
// PRESENTATION-FREE + C++20-clean.

#include <cmath>
#include <optional>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <Arcane/Physics/PhysicsTypes.hpp>
#include <Arcane/Physics/Shapes.hpp>
#include <Arcane/Physics/PhysicsWorld.hpp>
#include <Arcane/Physics/Broadphase/Passability.hpp>

using namespace Arcane::Physics;
using Catch::Approx;

namespace
{
    // A 16x16 Cartesian grid, cellSize 10, origin (0,0). Cell C spans world
    // [C*10, (C+1)*10) on each axis (min-corner indexing, matching
    // TileGrid::CellBox). The center of cell (cx,cy) is (cx*10+5, cy*10+5).
    constexpr int  kGridW    = 16;
    constexpr int  kGridH    = 16;
    constexpr Real kCellSize = Real(10);

    // World position of cell (cx,cy)'s center.
    Vec2 CellCenter(int cx, int cy)
    {
        return Vec2(static_cast<Real>(cx) * kCellSize + kCellSize * Real(0.5),
                    static_cast<Real>(cy) * kCellSize + kCellSize * Real(0.5));
    }

    WorldDef MakeWorldDef(const IPassabilitySource& src)
    {
        WorldDef def;
        def.passability  = &src;
        def.tileCellSize = kCellSize;
        def.tileOrigin   = Vec2(Real(0), Real(0));
        return def;
    }
}

// ---------------------------------------------------------------------------
// Raycast vs cells (Cartesian self-defined oracle)
// ---------------------------------------------------------------------------

TEST_CASE("Raycast: a wall cell in the path is hit with 0<t<1 + correct cell",
          "[physics][queries][raycast]")
{
    GridPassability grid(kGridW, kGridH);
    // Wall at cell (5, 2). A horizontal ray along row 2 must hit it.
    grid.SetSolid(5, 2, true);

    PhysicsWorld w(MakeWorldDef(grid));

    // Ray along the center of row 2, from cell 0 to cell 10 (x: 5 -> 105).
    const Vec2 from = CellCenter(0, 2);   // (5, 25)
    const Vec2 to   = CellCenter(10, 2);  // (105, 25)

    const auto hit = w.Raycast(from, to);
    REQUIRE(hit.has_value());
    REQUIRE(hit->isCell);
    REQUIRE(hit->cellX == 5);
    REQUIRE(hit->cellY == 2);
    REQUIRE(hit->t > Real(0));
    REQUIRE(hit->t < Real(1));
    // The hit enters cell 5 at its left boundary x = 50; t = (50-5)/(105-5) = 0.45.
    REQUIRE(hit->t == Approx(Real(0.45)));
    REQUIRE(hit->point.x == Approx(Real(50)));
    REQUIRE(hit->point.y == Approx(Real(25)));
}

TEST_CASE("Raycast: a clear column yields no cell hit", "[physics][queries][raycast]")
{
    GridPassability grid(kGridW, kGridH);
    // Wall on a DIFFERENT row -- not in the ray's path.
    grid.SetSolid(5, 7, true);

    PhysicsWorld w(MakeWorldDef(grid));

    const Vec2 from = CellCenter(0, 2);
    const Vec2 to   = CellCenter(10, 2);

    // cellsOnly so no bodies interfere; clear row -> no hit.
    RaycastOpts opts;
    opts.cellsOnly = true;
    const auto hit = w.Raycast(from, to, opts);
    REQUIRE_FALSE(hit.has_value());
}

TEST_CASE("Raycast: degenerate (zero-length) ray returns none",
          "[physics][queries][raycast]")
{
    GridPassability grid(kGridW, kGridH);
    grid.SetSolid(2, 2, true);
    PhysicsWorld w(MakeWorldDef(grid));

    const Vec2 p = CellCenter(2, 2);
    const auto hit = w.Raycast(p, p); // from == to
    REQUIRE_FALSE(hit.has_value());
}

TEST_CASE("Raycast: a diagonal wall cell is hit on the correct cell",
          "[physics][queries][raycast]")
{
    GridPassability grid(kGridW, kGridH);
    // Diagonal wall at (3,3). A 45-degree ray along the diagonal hits it.
    grid.SetSolid(3, 3, true);
    PhysicsWorld w(MakeWorldDef(grid));

    RaycastOpts opts;
    opts.cellsOnly = true;
    const Vec2 from = CellCenter(0, 0); // (5, 5)
    const Vec2 to   = CellCenter(8, 8); // (85, 85)
    const auto hit = w.Raycast(from, to, opts);
    REQUIRE(hit.has_value());
    REQUIRE(hit->isCell);
    REQUIRE(hit->cellX == 3);
    REQUIRE(hit->cellY == 3);
    REQUIRE(hit->t > Real(0));
    REQUIRE(hit->t < Real(1));
}

// ---------------------------------------------------------------------------
// Raycast vs bodies (coordinate-agnostic invariants)
// ---------------------------------------------------------------------------

TEST_CASE("Raycast: AABB body slab hit is exact", "[physics][queries][raycast]")
{
    // No passability source -> no cell pass; only the body test runs.
    PhysicsWorld w;

    BodyDef bd;
    bd.type     = BodyType::Static;
    bd.position = Vec2(Real(100), Real(0));
    bd.shape    = MakeAabb(Real(10), Real(10)); // box spans x[90,110], y[-10,10]
    BodyHandle box = w.AddBody(bd);

    // Horizontal ray from x=0 to x=200 at y=0 -> enters the box at x=90.
    const auto hit = w.Raycast(Vec2(Real(0), Real(0)), Vec2(Real(200), Real(0)));
    REQUIRE(hit.has_value());
    REQUIRE_FALSE(hit->isCell);
    REQUIRE(hit->body == box);
    // Enters at x=90; t = 90/200 = 0.45.
    REQUIRE(hit->t == Approx(Real(0.45)));
    REQUIRE(hit->point.x == Approx(Real(90)));
    REQUIRE(hit->point.y == Approx(Real(0)));
}

TEST_CASE("Raycast: a near body beats a farther wall (nearest wins)",
          "[physics][queries][raycast]")
{
    GridPassability grid(kGridW, kGridH);
    // Wall FAR down the row at cell (10, 2).
    grid.SetSolid(10, 2, true);
    PhysicsWorld w(MakeWorldDef(grid));

    // A static AABB body NEARER than the wall: spans x[40,60] on row-2 center.
    BodyDef bd;
    bd.type     = BodyType::Static;
    bd.position = Vec2(Real(50), Real(25)); // row 2 center y = 25
    bd.shape    = MakeAabb(Real(10), Real(10)); // x[40,60], y[15,35]
    BodyHandle box = w.AddBody(bd);

    const Vec2 from = CellCenter(0, 2);   // (5, 25)
    const Vec2 to   = CellCenter(14, 2);  // (145, 25)

    const auto hit = w.Raycast(from, to);
    REQUIRE(hit.has_value());
    // The body (enters x=40, t~0.25) beats the wall cell 10 (enters x=100,
    // t~0.68): the NEARER hit wins, and it is the body, not a cell.
    REQUIRE_FALSE(hit->isCell);
    REQUIRE(hit->body == box);
    REQUIRE(hit->point.x == Approx(Real(40)));
}

TEST_CASE("Raycast: a farther body loses to a nearer wall (nearest wins, "
          "cell side)", "[physics][queries][raycast]")
{
    GridPassability grid(kGridW, kGridH);
    // Wall NEAR at cell (2, 2): entered at x=20.
    grid.SetSolid(2, 2, true);
    PhysicsWorld w(MakeWorldDef(grid));

    // A body FARTHER than the wall: spans x[90,110].
    BodyDef bd;
    bd.type     = BodyType::Static;
    bd.position = Vec2(Real(100), Real(25));
    bd.shape    = MakeAabb(Real(10), Real(10));
    w.AddBody(bd);

    const Vec2 from = CellCenter(0, 2);   // (5, 25)
    const Vec2 to   = CellCenter(14, 2);  // (145, 25)

    const auto hit = w.Raycast(from, to);
    REQUIRE(hit.has_value());
    // The wall (x=20) is nearer than the body (x=90): the cell wins.
    REQUIRE(hit->isCell);
    REQUIRE(hit->cellX == 2);
    REQUIRE(hit->cellY == 2);
    REQUIRE(hit->point.x == Approx(Real(20)));
}

TEST_CASE("Raycast: a diagonal ray through a circle center hits at exactly r",
          "[physics][queries][raycast]")
{
    PhysicsWorld w; // no cells; body test only

    const Real r = Real(10);
    const Vec2 center(Real(100), Real(100));
    BodyDef bd;
    bd.type     = BodyType::Static;
    bd.position = center;
    bd.shape    = MakeCircle(r);
    w.AddBody(bd);

    // Diagonal ray heading straight at the circle center from the lower-left.
    const Vec2 from(Real(0), Real(0));
    const Vec2 to(Real(200), Real(200)); // passes through (100,100)

    const auto hit = w.Raycast(from, to);
    REQUIRE(hit.has_value());
    REQUIRE_FALSE(hit->isCell);
    // The harness "exact surface hit (r=10)" invariant: the hit point lies on
    // the circle surface, i.e. |hit.point - center| == r (within ~0.2 -- the
    // conservative-advancement TOL is 0.05).
    const Real dx = hit->point.x - center.x;
    const Real dy = hit->point.y - center.y;
    const Real dist = std::sqrt(dx * dx + dy * dy);
    REQUIRE(dist == Approx(r).margin(Real(0.2)));
    // And it hit the NEAR surface (the lower-left side), so the hit is before
    // the center along the ray.
    REQUIRE(hit->point.x < center.x);
    REQUIRE(hit->point.y < center.y);
}

// ---------------------------------------------------------------------------
// LineOfSight (the obstacle-class tiering seam)
// ---------------------------------------------------------------------------

TEST_CASE("LineOfSight: a TALL cell blocks sight", "[physics][queries][los]")
{
    GridPassability grid(kGridW, kGridH);
    // TALL obstacle: blocks movement AND sight.
    grid.SetSolid(5, 2, true);
    grid.SetBlocksSight(5, 2, true);
    PhysicsWorld w(MakeWorldDef(grid));

    const Vec2 from = CellCenter(0, 2);
    const Vec2 to   = CellCenter(10, 2);
    REQUIRE_FALSE(w.LineOfSight(from, to)); // blocked
}

TEST_CASE("LineOfSight: a LOW cell (see-through) does NOT block sight",
          "[physics][queries][los]")
{
    GridPassability grid(kGridW, kGridH);
    // LOW obstacle: blocks MOVEMENT (IsSolid true) but NOT sight
    // (BlocksSight false). The seam tiering: LineOfSight uses BlocksSight, so a
    // movement raycast would be blocked here but sight passes through.
    grid.SetSolid(5, 2, true);
    grid.SetBlocksSight(5, 2, false);
    PhysicsWorld w(MakeWorldDef(grid));

    const Vec2 from = CellCenter(0, 2);
    const Vec2 to   = CellCenter(10, 2);

    // Sight passes (LOW is see-through)...
    REQUIRE(w.LineOfSight(from, to));

    // ...but a MOVEMENT raycast (IsSolid) IS blocked by the same LOW cell.
    RaycastOpts mov;
    mov.cellsOnly = true; // movement rule = IsSolid (tallOnly false)
    const auto blocked = w.Raycast(from, to, mov);
    REQUIRE(blocked.has_value());
    REQUIRE(blocked->isCell);
    REQUIRE(blocked->cellX == 5);
    REQUIRE(blocked->cellY == 2);
}

TEST_CASE("LineOfSight: a clear column has sight", "[physics][queries][los]")
{
    GridPassability grid(kGridW, kGridH);
    grid.SetSolid(5, 9, true);
    grid.SetBlocksSight(5, 9, true); // TALL, but on a different row
    PhysicsWorld w(MakeWorldDef(grid));

    const Vec2 from = CellCenter(0, 2);
    const Vec2 to   = CellCenter(10, 2);
    REQUIRE(w.LineOfSight(from, to)); // clear
}

TEST_CASE("LineOfSight: degenerate segment has no sight", "[physics][queries][los]")
{
    GridPassability grid(kGridW, kGridH);
    PhysicsWorld w(MakeWorldDef(grid));
    const Vec2 p = CellCenter(3, 3);
    REQUIRE_FALSE(w.LineOfSight(p, p)); // ports `if not x1 then return false`
}

// ---------------------------------------------------------------------------
// ShapeCast (world) -- reuses the P1.4 GJK conservative-advancement primitive
// ---------------------------------------------------------------------------

TEST_CASE("ShapeCast: a swept circle stops a radius short of where a ray hits",
          "[physics][queries][shapecast]")
{
    PhysicsWorld w; // no cells; cast against a static body

    // A static wall AABB the cast moves toward: spans x[100,120], y[-50,50].
    BodyDef bd;
    bd.type     = BodyType::Static;
    bd.position = Vec2(Real(110), Real(0));
    bd.shape    = MakeAabb(Real(10), Real(50));
    w.AddBody(bd);

    const Real circleR = Real(8);
    const Shape circle = MakeCircle(circleR);
    const Vec2  start(Real(0), Real(0));
    const Vec2  delta(Real(200), Real(0)); // sweep right past the wall

    const auto cast = w.ShapeCast(circle, start, delta);
    REQUIRE(cast.has_value());
    REQUIRE(cast->body != kInvalidBody);

    // A zero-radius RAY along the same line hits the wall's near face at x=100,
    // t = 100/200 = 0.5. The radius-8 circle must stop ~its radius SHORT of
    // that (its center stops near x=92), so cast.t < ray.t.
    const auto ray = w.Raycast(start, Vec2(start.x + delta.x, start.y + delta.y));
    REQUIRE(ray.has_value());
    REQUIRE(cast->t < ray->t);

    // The cast reports a near-touching surface distance (< the cast TOL) and a
    // push-back normal pointing back toward the moving shape (i.e. -x, away
    // from the wall the circle is approaching from the left).
    REQUIRE(cast->distance < Real(0.1));
    REQUIRE(cast->normal.x < Real(0)); // points back toward the mover
    // The circle center at TOI is ~radius short of the wall face (x=100).
    REQUIRE(cast->point.x == Approx(Real(100) - circleR).margin(Real(0.2)));
}

TEST_CASE("ShapeCast: a clear cast misses", "[physics][queries][shapecast]")
{
    PhysicsWorld w;

    // A static body well off the cast's path.
    BodyDef bd;
    bd.type     = BodyType::Static;
    bd.position = Vec2(Real(100), Real(500)); // far away in y
    bd.shape    = MakeAabb(Real(10), Real(10));
    w.AddBody(bd);

    const Shape circle = MakeCircle(Real(5));
    const auto cast =
        w.ShapeCast(circle, Vec2(Real(0), Real(0)), Vec2(Real(50), Real(0)));
    REQUIRE_FALSE(cast.has_value());
}

TEST_CASE("ShapeCast: degenerate (zero-length) delta returns none",
          "[physics][queries][shapecast]")
{
    PhysicsWorld w;
    BodyDef bd;
    bd.type     = BodyType::Static;
    bd.position = Vec2(Real(10), Real(0));
    bd.shape    = MakeAabb(Real(10), Real(10));
    w.AddBody(bd);

    const Shape circle = MakeCircle(Real(5));
    const auto cast =
        w.ShapeCast(circle, Vec2(Real(0), Real(0)), Vec2(Real(0), Real(0)));
    REQUIRE_FALSE(cast.has_value());
}

TEST_CASE("ShapeCast: movers are obstacles only with opts.movers",
          "[physics][queries][shapecast]")
{
    PhysicsWorld w;

    // A KINEMATIC body in the cast path -- a mover, not a static.
    BodyDef bd;
    bd.type     = BodyType::Kinematic;
    bd.position = Vec2(Real(100), Real(0));
    bd.shape    = MakeAabb(Real(10), Real(50));
    w.AddBody(bd);

    const Shape circle = MakeCircle(Real(8));
    const Vec2  start(Real(0), Real(0));
    const Vec2  delta(Real(200), Real(0));

    // Without opts.movers, the kinematic body is NOT an obstacle -> miss.
    REQUIRE_FALSE(w.ShapeCast(circle, start, delta).has_value());

    // With opts.movers, it IS -> hit.
    ShapeCastOpts opts;
    opts.movers = true;
    const auto cast = w.ShapeCast(circle, start, delta, opts);
    REQUIRE(cast.has_value());
    REQUIRE(cast->body != kInvalidBody);
}

TEST_CASE("ShapeCast: opts.exclude skips the caster's own body",
          "[physics][queries][shapecast]")
{
    PhysicsWorld w;

    // The "caster" body overlapping the start, plus a wall down the path.
    BodyDef self;
    self.type     = BodyType::Kinematic;
    self.position = Vec2(Real(0), Real(0));
    self.shape    = MakeCircle(Real(8));
    BodyHandle selfH = w.AddBody(self);

    BodyDef wall;
    wall.type     = BodyType::Kinematic;
    wall.position = Vec2(Real(100), Real(0));
    wall.shape    = MakeAabb(Real(10), Real(50));
    w.AddBody(wall);

    const Shape circle = MakeCircle(Real(8));
    ShapeCastOpts opts;
    opts.movers  = true;
    opts.exclude = selfH;

    const auto cast =
        w.ShapeCast(circle, Vec2(Real(0), Real(0)), Vec2(Real(200), Real(0)), opts);
    REQUIRE(cast.has_value());
    // The hit is the wall, not the excluded self body.
    REQUIRE(cast->body != selfH);
}

// ---------------------------------------------------------------------------
// OverlapShape
// ---------------------------------------------------------------------------

TEST_CASE("OverlapShape: returns exactly the overlapping bodies",
          "[physics][queries][overlap]")
{
    PhysicsWorld w;

    // Body A: overlaps the query. Body B: also overlaps. Body C: far away.
    BodyDef a;
    a.type     = BodyType::Static;
    a.position = Vec2(Real(0), Real(0));
    a.shape    = MakeCircle(Real(10));
    BodyHandle ha = w.AddBody(a);

    BodyDef b;
    b.type     = BodyType::Kinematic;
    b.position = Vec2(Real(15), Real(0));
    b.shape    = MakeCircle(Real(10));
    BodyHandle hb = w.AddBody(b);

    BodyDef c;
    c.type     = BodyType::Static;
    c.position = Vec2(Real(500), Real(500)); // far away -> excluded
    c.shape    = MakeCircle(Real(10));
    w.AddBody(c);

    // Query a circle centered at (5,0) radius 12: overlaps A (dist 5 < 22) and
    // B (dist 10 < 22), excludes C.
    const Shape query = MakeCircle(Real(12));
    const Transform xf{ Vec2(Real(5), Real(0)), Real(0) };

    std::vector<BodyHandle> hits;
    const int n = w.OverlapShape(query, xf, hits);
    REQUIRE(n == 2);
    // Index-ordered: A added first, then B.
    REQUIRE(hits[0] == ha);
    REQUIRE(hits[1] == hb);
}

TEST_CASE("OverlapShape: a non-overlapping query returns empty",
          "[physics][queries][overlap]")
{
    PhysicsWorld w;

    BodyDef a;
    a.type     = BodyType::Static;
    a.position = Vec2(Real(0), Real(0));
    a.shape    = MakeCircle(Real(5));
    w.AddBody(a);

    const Shape query = MakeCircle(Real(5));
    const Transform xf{ Vec2(Real(100), Real(100)), Real(0) }; // far away

    std::vector<BodyHandle> hits;
    const int n = w.OverlapShape(query, xf, hits);
    REQUIRE(n == 0);
    REQUIRE(hits.empty());
}

// ---------------------------------------------------------------------------
// QueryAABB (P1.8 coverage + a non-overlap exclusion gap)
// ---------------------------------------------------------------------------

TEST_CASE("QueryAABB: includes overlapping, excludes non-overlapping",
          "[physics][queries][aabb]")
{
    PhysicsWorld w;

    BodyDef in;
    in.type     = BodyType::Kinematic;
    in.position = Vec2(Real(10), Real(10));
    in.shape    = MakeCircle(Real(5));
    BodyHandle hin = w.AddBody(in);

    BodyDef out;
    out.type     = BodyType::Static;
    out.position = Vec2(Real(500), Real(500));
    out.shape    = MakeAabb(Real(5), Real(5));
    w.AddBody(out);

    std::vector<BodyHandle> hits;
    const int n =
        w.QueryAABB(Aabb{ Vec2(Real(0), Real(0)), Vec2(Real(50), Real(50)) }, hits);
    REQUIRE(n == 1);
    REQUIRE(hits[0] == hin);
}
