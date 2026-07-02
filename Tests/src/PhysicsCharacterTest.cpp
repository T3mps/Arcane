// Physics M6 P1.10: CharacterController -- WASD slide + click-to-move.
//
// PORT NOTE: a BEHAVIORAL port of the physics_harness CharacterController block
// (Client/src/tests/physics_harness/main.lua ~458-555). The harness block is
// ISO-COUPLED (Iso.cellCenter spawns, an iso TileGrid), so we DO NOT bit-match
// its iso numbers. Instead we REFORMULATE to a SELF-DEFINED CARTESIAN world
// (GridPassability + a Cartesian TileGrid) and assert the SAME behavioral
// invariants the harness actually checks -- these ARE the parity surface for
// this layer (the algorithm + the invariants, not iso coords):
//
//   * Slam into a wall: repeated SlideMove toward a wall ADVANCES the body
//     toward the wall before stopping (final pos past the spawn, short of the
//     wall); maxPenetration < 0.5 at every tick (never buried).
//   * Big-dt no-tunnel: one giant SlideMove (64px) toward an interior wall cell
//     does NOT tunnel through it and does NOT bury (maxPenetration < 0.5).
//   * Slide along a face: diagonal input into a wall face still makes progress
//     (moved > ~10px over N ticks) and stays out (maxPenetration < 0.5).
//   * Tile-seam slide (keystone): slide DOWN a column of single-cell wall spans
//     (one rect per row -> a span seam every row); the capsule rides over every
//     seam without catching and travels MULTIPLE rows (net displacement spans
//     > 2 cell-rows). maxPenetration < 0.5 throughout. (Validates the per-row
//     merged TileGrid + depenetration don't catch on seams.)
//   * Circle body drives the CC (zero-length capsule path): slam a circle body
//     into a wall; it advances and never buries (< 0.5).
//
// The maxPenetration helper mirrors the controller's deepest-capsule-penetration
// probe: StaticCandidates near the body -> CapsulePoly vs the solid tile spans,
// keeping the deepest depth. (The harness probes the same way.)
//
// PRESENTATION-FREE + C++20-clean.

#include <array>
#include <cmath>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <Arcane/Physics/PhysicsTypes.hpp>
#include <Arcane/Physics/Shapes.hpp>
#include <Arcane/Physics/PhysicsWorld.hpp>
#include <Arcane/Physics/CharacterController.hpp>
#include <Arcane/Physics/Broadphase/Passability.hpp>
#include <Arcane/Physics/Narrowphase/GeometryKernel.hpp>

using namespace Arcane::Physics;
using Catch::Approx;

namespace
{
    // A Cartesian grid, cellSize 16, origin (0,0). Cell C spans world
    // [C*16, (C+1)*16) on each axis; cell (cx,cy) center = (cx*16+8, cy*16+8).
    constexpr int  kGridW    = 24;
    constexpr int  kGridH    = 24;
    constexpr Real kCellSize = Real(16);

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
        def.gravityX               = Real(0);   // PX-PIN: remove when this file converts to MKS
        def.gravityY               = Real(0);   // PX-PIN: remove when this file converts to MKS
        def.sleepThreshold         = Real(8);   // PX-PIN: remove when this file converts to MKS
        def.restitutionThreshold   = Real(20);  // PX-PIN: remove when this file converts to MKS
        def.contactPushMaxVelocity = Real(300); // PX-PIN: remove when this file converts to MKS
        def.hashCellSize           = Real(64);  // PX-PIN: remove when this file converts to MKS
        return def;
    }

    // A border wall ring around [0,W)x[0,H), so the interior is enclosed.
    void AddBorderRing(GridPassability& grid)
    {
        for (int x = 0; x < kGridW; ++x)
        {
            grid.SetSolid(x, 0, true);
            grid.SetSolid(x, kGridH - 1, true);
        }
        for (int y = 0; y < kGridH; ++y)
        {
            grid.SetSolid(0, y, true);
            grid.SetSolid(kGridW - 1, y, true);
        }
    }

    // Deepest capsule penetration of the body at handle `h` against the SOLID
    // tile spans near it (the controller's own probe). Mirrors _depenetrate's
    // span loop: StaticCandidates -> CapsulePoly per span-as-poly, keep deepest.
    // spans only; static bodies not probed (these scenarios use only
    // TileGrid/GridPassability).
    Real MaxPenetration(const PhysicsWorld& w, BodyHandle h)
    {
        const Shape* sp = w.GetShape(h);
        if (sp == nullptr)
        {
            return Real(0);
        }
        const Shape& s = *sp;
        const Vec2 pos = w.Position(h);
        const Real halfLen = (s.kind == ShapeKind::Capsule) ? s.halfLen : Real(0);
        const Real r       = s.radius;

        const Transform xf{ pos, Real(0) };
        const Aabb2 box = s.ComputeAABB(xf);

        std::vector<Aabb2>         spans;
        std::vector<std::uint32_t> statics;
        std::vector<std::uint32_t> gridScratch;
        w.StaticCandidates(box, spans, statics, gridScratch);

        const Vec2 segA(pos.x - halfLen, pos.y);
        const Vec2 segB(pos.x + halfLen, pos.y);

        Real worst = Real(0);
        for (const Aabb2& span : spans)
        {
            std::array<Vec2, 4> corners{
                Vec2(span.min.x, span.min.y), Vec2(span.max.x, span.min.y),
                Vec2(span.max.x, span.max.y), Vec2(span.min.x, span.max.y)
            };
            const Hit hit = CapsulePoly(segA, segB, r, corners.data(), 4);
            if (hit.hit && hit.depth > worst)
            {
                worst = hit.depth;
            }
        }
        return worst;
    }

    // Spawn a kinematic capsule player at world `pos`.
    BodyHandle SpawnCapsule(PhysicsWorld& w, Vec2 pos, Real halfLen, Real r)
    {
        BodyDef def;
        def.type     = BodyType::Kinematic;
        def.position = pos;
        def.shape    = MakeCapsule(halfLen, r);
        return w.AddBody(def);
    }

    BodyHandle SpawnCircle(PhysicsWorld& w, Vec2 pos, Real r)
    {
        BodyDef def;
        def.type     = BodyType::Kinematic;
        def.position = pos;
        def.shape    = MakeCircle(r);
        return w.AddBody(def);
    }
}

// ---------------------------------------------------------------------------
// Slam into a wall: advance toward it, stop short, never bury.
// ---------------------------------------------------------------------------

TEST_CASE("CharacterController: slam into a wall advances then stops, never buries",
          "[physics][character]")
{
    GridPassability grid(kGridW, kGridH);
    AddBorderRing(grid);

    PhysicsWorld w(MakeWorldDef(grid));

    // Spawn well inside, a few cells left of the right border wall (col 23).
    // Spawn at col 18 center; the wall face is at x = 23*16 = 368.
    const Vec2 spawn = CellCenter(18, 12);
    BodyHandle h = SpawnCapsule(w, spawn, Real(6), Real(7));
    CharacterController cc(w, h);

    const Real spawnX = w.Position(h).x;
    const Real wallFaceX = static_cast<Real>(kGridW - 1) * kCellSize; // 368

    // Drive right hard, many ticks. Order: Step FIRST then SlideMove (contract).
    for (int tick = 0; tick < 200; ++tick)
    {
        w.Step(Real(1.0 / 60.0));
        (void)cc.SlideMove(Real(20), Real(0)); // 20px/tick to the right
        REQUIRE(MaxPenetration(w, h) < Real(0.5));
    }

    const Real finalX = w.Position(h).x;
    // Advanced toward the wall...
    REQUIRE(finalX > spawnX + Real(20));
    // ...but stopped short of being buried in / past the wall face (capsule
    // half-width 6 + radius 7 = 13, so its right tip must stay <= wallFaceX +
    // a sliver of skin).
    REQUIRE(finalX < wallFaceX);
    // Velocity stays 0 in WASD mode (Step must not double-move the slid body).
    REQUIRE(w.Velocity(h).x == Approx(Real(0)));
    REQUIRE(w.Velocity(h).y == Approx(Real(0)));
}

// ---------------------------------------------------------------------------
// Big-dt no-tunnel: one giant SlideMove must not tunnel an interior wall cell.
// ---------------------------------------------------------------------------

TEST_CASE("CharacterController: big-dt slide does not tunnel an interior wall",
          "[physics][character]")
{
    GridPassability grid(kGridW, kGridH);
    AddBorderRing(grid);
    // Interior wall cell at (12, 12).
    grid.SetSolid(12, 12, true);

    PhysicsWorld w(MakeWorldDef(grid));

    // Spawn 4 cells left of the wall cell center, same row.
    const Vec2 spawn = CellCenter(8, 12);
    BodyHandle h = SpawnCapsule(w, spawn, Real(5), Real(6));
    CharacterController cc(w, h);

    const Real spawnX    = w.Position(h).x;
    const Real wallMinX  = static_cast<Real>(12) * kCellSize;      // 192 (cell left face)

    w.Step(Real(1.0 / 60.0));
    // One giant 64px displacement straight at the wall cell.
    (void)cc.SlideMove(Real(64), Real(0));

    // Did NOT tunnel through: the body's center must be left of the wall cell's
    // far side (it stopped at the near face, not popped out the other side).
    const Real finalX = w.Position(h).x;
    REQUIRE(finalX > spawnX);          // it advanced
    REQUIRE(finalX < wallMinX);        // it did not pass into / through the cell
    REQUIRE(MaxPenetration(w, h) < Real(0.5)); // not buried
}

// ---------------------------------------------------------------------------
// Slide along a face: diagonal into a wall still makes progress along it.
// ---------------------------------------------------------------------------

TEST_CASE("CharacterController: diagonal into a wall face slides along it",
          "[physics][character]")
{
    GridPassability grid(kGridW, kGridH);
    AddBorderRing(grid);

    PhysicsWorld w(MakeWorldDef(grid));

    // Spawn near the right border wall, mid-height. Push down-and-right: the
    // rightward component is blocked by the wall, the downward component should
    // still carry the body along the wall face.
    const Vec2 spawn = CellCenter(21, 6);
    BodyHandle h = SpawnCapsule(w, spawn, Real(5), Real(6));
    CharacterController cc(w, h);

    const Real startY = w.Position(h).y;

    for (int tick = 0; tick < 60; ++tick)
    {
        w.Step(Real(1.0 / 60.0));
        (void)cc.SlideMove(Real(14), Real(14)); // into the wall + downward
        REQUIRE(MaxPenetration(w, h) < Real(0.5));
    }

    const Real movedY = w.Position(h).y - startY;
    // Made meaningful progress DOWN the face despite the wall blocking right.
    REQUIRE(movedY > Real(10));
}

// ---------------------------------------------------------------------------
// Tile-seam slide (keystone): ride down a column of single-cell spans.
// ---------------------------------------------------------------------------

TEST_CASE("CharacterController: slides down a seamed wall column without catching",
          "[physics][character]")
{
    GridPassability grid(kGridW, kGridH);
    AddBorderRing(grid);

    // A vertical wall column at cx = 10, spanning many rows. Each row is a
    // SEPARATE single-cell span (TileGrid merges only horizontally, so a 1-wide
    // column yields one rect per row -> a span SEAM at every row boundary). The
    // capsule rides just to the LEFT of this column and slides straight down,
    // crossing a seam every row.
    const int colX = 10;
    for (int cy = 1; cy < kGridH - 1; ++cy)
    {
        grid.SetSolid(colX, cy, true);
    }

    PhysicsWorld w(MakeWorldDef(grid));

    // Spawn just left of the column, high up. The column's left face is at
    // x = colX*16 = 160. Place the body so it lightly presses the face.
    const Real colFaceX = static_cast<Real>(colX) * kCellSize; // 160
    const Real r        = Real(6);
    const Vec2 spawn(colFaceX - r - Real(1), CellCenter(0, 3).y);
    BodyHandle h = SpawnCapsule(w, spawn, Real(5), r);
    CharacterController cc(w, h);

    const Real startY = w.Position(h).y;

    // Push down-and-into-the-wall every tick so the body hugs the seamed face
    // while descending. Must ride over EVERY row seam without catching.
    for (int tick = 0; tick < 120; ++tick)
    {
        w.Step(Real(1.0 / 60.0));
        (void)cc.SlideMove(Real(6), Real(18)); // into the face + strong downward
        REQUIRE(MaxPenetration(w, h) < Real(0.5));
    }

    const Real movedY = w.Position(h).y - startY;
    // Traveled MULTIPLE cell-rows down (net displacement spans > 2 rows) -- it
    // never caught on a seam.
    REQUIRE(movedY > kCellSize * Real(2));
}

// ---------------------------------------------------------------------------
// Circle body drives the CC (zero-length capsule path).
// ---------------------------------------------------------------------------

TEST_CASE("CharacterController: a circle body slams a wall and never buries",
          "[physics][character]")
{
    GridPassability grid(kGridW, kGridH);
    AddBorderRing(grid);

    PhysicsWorld w(MakeWorldDef(grid));

    const Vec2 spawn = CellCenter(18, 12);
    BodyHandle h = SpawnCircle(w, spawn, Real(7)); // zero-length capsule path
    CharacterController cc(w, h);

    const Real spawnX    = w.Position(h).x;
    const Real wallFaceX = static_cast<Real>(kGridW - 1) * kCellSize; // 368

    for (int tick = 0; tick < 200; ++tick)
    {
        w.Step(Real(1.0 / 60.0));
        (void)cc.SlideMove(Real(20), Real(0));
        REQUIRE(MaxPenetration(w, h) < Real(0.5));
    }

    const Real finalX = w.Position(h).x;
    REQUIRE(finalX > spawnX + Real(20)); // advanced
    REQUIRE(finalX < wallFaceX);         // stopped short of the wall
}

// ---------------------------------------------------------------------------
// FollowVelocity: click mode sets velocity (integrated by the NEXT Step).
// ---------------------------------------------------------------------------

TEST_CASE("CharacterController: FollowVelocity sets the kinematic velocity",
          "[physics][character]")
{
    GridPassability grid(kGridW, kGridH);
    AddBorderRing(grid);

    PhysicsWorld w(MakeWorldDef(grid));

    const Vec2 spawn = CellCenter(6, 6);
    BodyHandle h = SpawnCapsule(w, spawn, Real(5), Real(6));
    CharacterController cc(w, h);

    cc.FollowVelocity(Real(30), Real(-12));
    REQUIRE(w.Velocity(h).x == Approx(Real(30)));
    REQUIRE(w.Velocity(h).y == Approx(Real(-12)));

    // The NEXT Step integrates it (no deflection): pos advances by v*dt.
    const Vec2 before = w.Position(h);
    const Real dt = Real(1.0 / 60.0);
    w.Step(dt);
    const Vec2 after = w.Position(h);
    REQUIRE(after.x == Approx(before.x + Real(30) * dt).margin(1e-4));
    REQUIRE(after.y == Approx(before.y + Real(-12) * dt).margin(1e-4));
}
