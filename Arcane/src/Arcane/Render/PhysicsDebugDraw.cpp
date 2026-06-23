// PhysicsDebugDraw.cpp -- physics debug-draw overlay (M6, Task P3.6).
//
// Ports Client/src/physics/PhysicsDebug.lua into the Arcane.dll render side.
// Consumes the Core pull API (PhysicsWorld::ForEachContact / IslandRootOf /
// Count / Alive / ShapeSlot / PosSlot / TypeSlot / SensorSlot / AwakeSlot /
// SlotAabb / GetAngle) and submits Batcher2D primitives (Line, Circle, Rect).
//
// MODERNIZATION: dynamic bodies are colored by island (IslandRootOf keyed
// into a small hue palette) instead of the Lua's uniform kinematic green.
// Sleeping dynamics are drawn at 35% brightness (Lua dim = 0.35 branch).
//
// PRESENTATION BOUNDARY (one-way): Core -> never includes Render. This file
// lives in Arcane.dll and is the only permitted side to couple physics + render.

#include <Arcane/Render/PhysicsDebugDraw.hpp>

#include <cmath>
#include <cstdint>

#include <glm/glm.hpp>
#include <glm/trigonometric.hpp>

#include <unordered_map>
#include <vector>

#include <Arcane/Physics/Broadphase/Broadphase.hpp>       // BroadphasePair, Aabb2
#include <Arcane/Physics/Broadphase/DynamicTree.hpp>      // FixtureBroadphaseTree / ForEachLeaf
#include <Arcane/Physics/Broadphase/SpatialGrid.hpp>      // StaticGrid / ResidencyGrid / ForEachCell
#include <Arcane/Physics/Narrowphase/NarrowphaseTrace.hpp> // NarrowphaseKind
#include <Arcane/Physics/PhysicsTypes.hpp>
#include <Arcane/Physics/PhysicsWorld.hpp>
#include <Arcane/Physics/Shapes.hpp>
#include <Arcane/Physics/Solver/Solver.hpp>               // ContactConstraint
#include <Arcane/Render/Batcher2D.hpp>

namespace Arcane
{
    namespace
    {
        // ---- port of PhysicsDebug.lua color constants -----------------------

        // glm::vec4 (r, g, b, a) in linear float; may be HDR (no clamp).
        constexpr glm::vec4 kColKinematic{ 0.2f, 1.0f, 0.4f, 1.0f };
        constexpr glm::vec4 kColStatic   { 0.4f, 0.7f, 1.0f, 1.0f };
        constexpr glm::vec4 kColSensor   { 1.0f, 0.9f, 0.2f, 0.9f };
        constexpr glm::vec4 kColContact  { 1.0f, 0.2f, 1.0f, 1.0f }; // magenta
        constexpr glm::vec4 kColAabb     { 1.0f, 1.0f, 1.0f, 0.4f }; // white dim

        // ---- island palette -------------------------------------------------
        //
        // 8-color palette keyed by (islandRoot % 8).  These are distinct hues so
        // different islands are visually separate at a glance.
        constexpr glm::vec4 kIslandPalette[8] =
        {
            { 1.0f, 0.35f, 0.35f, 1.0f }, // red
            { 1.0f, 0.65f, 0.15f, 1.0f }, // orange
            { 1.0f, 1.00f, 0.20f, 1.0f }, // yellow
            { 0.2f, 0.95f, 0.35f, 1.0f }, // green
            { 0.2f, 0.80f, 1.00f, 1.0f }, // cyan
            { 0.5f, 0.35f, 1.00f, 1.0f }, // blue-violet
            { 1.0f, 0.30f, 0.90f, 1.0f }, // pink
            { 0.85f,0.85f, 0.85f, 1.0f }, // light grey (island 7 / fallback)
        };

        // ---- helpers --------------------------------------------------------

        // Apply the camera to a world-space position -> canvas position.
        // CANONICAL transform (matches RenderSubmissionSystem +
        // Sandbox::Camera::WorldToScreen): screen = world * zoom + offset.
        inline glm::vec2 ToScreen(const Physics::Vec2& wpos,
                                  const glm::vec2& offset,
                                  float zoom)
        {
            return glm::vec2(static_cast<float>(wpos.x) * zoom + offset.x,
                             static_cast<float>(wpos.y) * zoom + offset.y);
        }

        // Draw an AABB outline using four lines.
        inline void DrawAabbOutline(Batcher2D& b,
                                    const Physics::Aabb& aabb,
                                    const glm::vec2& off,
                                    float zoom,
                                    float thickness,
                                    const glm::vec4& color)
        {
            const glm::vec2 mn = glm::vec2(static_cast<float>(aabb.min.x),
                                           static_cast<float>(aabb.min.y)) * zoom + off;
            const glm::vec2 mx = glm::vec2(static_cast<float>(aabb.max.x),
                                           static_cast<float>(aabb.max.y)) * zoom + off;
            const glm::vec2 bl = glm::vec2(mn.x, mx.y);
            const glm::vec2 tr = glm::vec2(mx.x, mn.y);

            b.Line(mn, tr, thickness, color); // top
            b.Line(tr, mx, thickness, color); // right
            b.Line(mx, bl, thickness, color); // bottom
            b.Line(bl, mn, thickness, color); // left
        }

        // Rotate a 2D point by `angle` radians around the origin.
        inline glm::vec2 Rotate2D(glm::vec2 v, float angle)
        {
            const float s = std::sin(angle);
            const float c = std::cos(angle);
            return glm::vec2(v.x * c - v.y * s, v.x * s + v.y * c);
        }

        // ---- rich-overlay colors (Sandbox outline-unify pivot) --------------
        constexpr glm::vec4 kColVelocity{ 0.20f, 1.00f, 0.55f, 1.0f }; // green ray
        constexpr glm::vec4 kColCom     { 1.00f, 1.00f, 1.00f, 1.0f }; // white cross
        constexpr glm::vec4 kColOrient  { 1.00f, 0.55f, 0.15f, 1.0f }; // orange tick

        // ---- Slice A broadphase + manifold colors ---------------------------
        //
        // Distinct hues so the three broadphase overlays read apart from each
        // other and from the per-body outlines: the DynamicTree leaves are cyan
        // (tight bright, fat dim/translucent), broadphase candidate-pair links are
        // a brighter cyan, the static grid is a cool blue, the residency grid is a
        // warm amber (static vs residency read differently at a glance).
        constexpr glm::vec4 kColTreeTight{ 0.30f, 0.90f, 1.00f, 0.85f }; // cyan, bright
        constexpr glm::vec4 kColTreeFat  { 0.30f, 0.90f, 1.00f, 0.25f }; // cyan, dim (fat)
        constexpr glm::vec4 kColTreePair { 0.20f, 1.00f, 0.90f, 0.80f }; // teal pair link
        constexpr glm::vec4 kColStaticGrid   { 0.35f, 0.55f, 1.00f, 0.35f }; // cool blue
        constexpr glm::vec4 kColResidencyGrid{ 1.00f, 0.70f, 0.20f, 0.35f }; // warm amber

        // Manifold normal-arrow length (world units, pre-zoom) + contact-point
        // disc radius (canvas px, post-zoom-independent for visibility).
        constexpr float kManifoldNormalLen = 20.0f; // world units
        constexpr float kManifoldPointPx   = 3.0f;   // canvas px

        // Fixed debug palette keyed by NarrowphaseKind, so a contact's manifold is
        // colored by the narrowphase path that produced it (a glance tells you
        // which collide branch fired). Separated never draws (no points), but it
        // gets a neutral grey so the lookup is total.
        inline glm::vec4 ManifoldColor(Physics::NarrowphaseKind kind)
        {
            switch (kind)
            {
                case Physics::NarrowphaseKind::CircleCircle:    return { 1.00f, 0.30f, 0.30f, 1.0f }; // red
                case Physics::NarrowphaseKind::CircleVsPolygon: return { 1.00f, 0.65f, 0.15f, 1.0f }; // orange
                case Physics::NarrowphaseKind::Capsule:         return { 1.00f, 1.00f, 0.25f, 1.0f }; // yellow
                case Physics::NarrowphaseKind::SatPolygon:      return { 0.30f, 1.00f, 0.45f, 1.0f }; // green
                case Physics::NarrowphaseKind::Epa:             return { 0.40f, 0.70f, 1.00f, 1.0f }; // blue
                case Physics::NarrowphaseKind::Mpr:             return { 0.80f, 0.45f, 1.00f, 1.0f }; // violet
                case Physics::NarrowphaseKind::Separated:
                default:                                        return { 0.70f, 0.70f, 0.70f, 1.0f }; // grey
            }
        }

        // World-space center of mass as a glm::vec2: bodyPos + R(angle) *
        // localCenter. For a single-fixture body localCenter is (0,0), so the
        // COM is the origin. (Named ComWorldF to avoid colliding with the Core
        // Physics::WorldCom that `using namespace Physics` pulls in.)
        inline glm::vec2 ComWorldF(const Physics::Vec2& pos, float angle,
                                   const Physics::Vec2& localCenter)
        {
            const glm::vec2 lc(static_cast<float>(localCenter.x),
                               static_cast<float>(localCenter.y));
            return glm::vec2(static_cast<float>(pos.x), static_cast<float>(pos.y))
                 + Rotate2D(lc, angle);
        }

        // Draw a short arrow head at `tip`, opening back toward `from`.
        inline void DrawArrowHead(Batcher2D& b, const glm::vec2& from,
                                  const glm::vec2& tip, float thickness,
                                  const glm::vec4& color)
        {
            glm::vec2 dir = tip - from;
            const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y);
            if (len < 1e-4f) return;
            dir /= len;
            const glm::vec2 perp(-dir.y, dir.x);
            const float head = (len < 12.0f) ? len * 0.5f : 6.0f; // px
            const glm::vec2 base = tip - dir * head;
            b.Line(tip, base + perp * (head * 0.6f), thickness, color);
            b.Line(tip, base - perp * (head * 0.6f), thickness, color);
        }

    } // anonymous namespace

    // -------------------------------------------------------------------------
    // DrawPhysicsDebug
    // -------------------------------------------------------------------------

    void DrawPhysicsDebug(const Physics::PhysicsWorld& world,
                          Batcher2D& batcher,
                          const PhysicsDebugDrawOptions& opts)
    {
        using namespace Physics;

        const glm::vec2& off     = opts.cameraOffset;
        const float      zoom    = opts.zoom;
        const float      thick   = opts.lineThickness;
        const std::uint32_t n    = world.Count();

        // ---- per-body shape outlines ----------------------------------------
        for (std::uint32_t i = 0; i < n; ++i)
        {
            if (!world.Alive(i))
                continue;

            const Shape&    s      = world.ShapeSlot(i);
            const Vec2      wpos   = world.PosSlot(i);
            const BodyType  btype  = world.TypeSlot(i);
            const bool      sensor = world.SensorSlot(i);
            const bool      awake  = world.AwakeSlot(i);
            const glm::vec2 spos   = ToScreen(wpos, off, zoom);

            // ---- color selection (port of PhysicsDebug.lua lines 29-34) ----
            glm::vec4 col;
            if (sensor)
            {
                col = kColSensor;
            }
            else if (btype == BodyType::Static)
            {
                col = kColStatic;
            }
            else if (btype == BodyType::Dynamic)
            {
                // Color by island root (the modernization).
                const std::uint32_t root = world.IslandRootOf(i);
                col = kIslandPalette[root % 8u];

                // Sleeping dynamic bodies drawn dim (Lua dim = 0.35).
                if (!awake)
                {
                    constexpr float kDim = 0.35f;
                    col.r *= kDim;
                    col.g *= kDim;
                    col.b *= kDim;
                }
            }
            else
            {
                // Kinematic.
                col = kColKinematic;
            }

            // ---- shape dispatch (port of PhysicsDebug.lua lines 34-44) -----
            switch (s.kind)
            {
                case ShapeKind::Circle:
                {
                    // Lua: lg.circle("line", x, y, s.r)
                    // Batcher2D::Circle is a filled SDF circle -- acceptable for
                    // debug; the filled disc at low alpha still reads as an
                    // outline at debug scale.
                    batcher.Circle(spos, static_cast<float>(s.radius) * zoom, col);
                    break;
                }

                case ShapeKind::Capsule:
                {
                    // Two end circles + two side lines. Capsule: segment
                    // (-halfLen,0)-(+halfLen,0) inflated by radius. Capsules rotate
                    // freely in v2, so rotate the local geometry by the body angle
                    // (mirrors the Polygon case): rotate the local point, then scale
                    // by zoom (rotation + uniform scale commute) and offset by spos.
                    const float angle = static_cast<float>(world.GetAngle(world.HandleOf(i)));
                    const float hl = static_cast<float>(s.halfLen); // local units
                    const float rl = static_cast<float>(s.radius);  // local units
                    const float r  = rl * zoom;                     // screen radius
                    const auto P = [&](float lx, float ly) {
                        return spos + Rotate2D(glm::vec2(lx, ly), angle) * zoom;
                    };
                    batcher.Circle(P(-hl, 0.0f), r, col);  // end caps at the rotated
                    batcher.Circle(P( hl, 0.0f), r, col);  // segment endpoints
                    batcher.Line(P(-hl, -rl), P(hl, -rl), thick, col); // side lines at
                    batcher.Line(P(-hl,  rl), P(hl,  rl), thick, col); // +/- radius, rotated
                    break;
                }

                case ShapeKind::Aabb:
                {
                    // Lua: lg.rectangle("line", x-hw, y-hh, hw*2, hh*2)
                    // Four lines forming the box outline.
                    const float hw = static_cast<float>(s.halfW) * zoom;
                    const float hh = static_cast<float>(s.halfH) * zoom;
                    const glm::vec2 tl(spos.x - hw, spos.y - hh);
                    const glm::vec2 tr(spos.x + hw, spos.y - hh);
                    const glm::vec2 br(spos.x + hw, spos.y + hh);
                    const glm::vec2 bl(spos.x - hw, spos.y + hh);
                    batcher.Line(tl, tr, thick, col);
                    batcher.Line(tr, br, thick, col);
                    batcher.Line(br, bl, thick, col);
                    batcher.Line(bl, tl, thick, col);
                    break;
                }

                case ShapeKind::Polygon:
                {
                    // Lua: lg.push(); lg.translate(x,y); lg.polygon("line", s.verts); lg.pop()
                    // Draw one line per edge; transform verts by body position + angle.
                    if (s.verts.empty())
                        break;
                    const float angle = static_cast<float>(world.GetAngle(world.HandleOf(i)));
                    const std::size_t vc = s.verts.size();
                    for (std::size_t e = 0; e < vc; ++e)
                    {
                        const Vec2& va = s.verts[e];
                        const Vec2& vb = s.verts[(e + 1) % vc];
                        // Rotate the local vert, then scale by zoom (rotation +
                        // uniform scale commute) so the polygon matches the
                        // world*zoom+offset transform of spos.
                        const glm::vec2 a = spos + Rotate2D(
                            glm::vec2(static_cast<float>(va.x),
                                      static_cast<float>(va.y)), angle) * zoom;
                        const glm::vec2 b2 = spos + Rotate2D(
                            glm::vec2(static_cast<float>(vb.x),
                                      static_cast<float>(vb.y)), angle) * zoom;
                        batcher.Line(a, b2, thick, col);
                    }
                    break;
                }
            }

            // ---- optional AABB outline (opts.drawAabbs) --------------------
            if (opts.drawAabbs)
            {
                DrawAabbOutline(batcher, world.SlotAabb(i), off, zoom, thick, kColAabb);
            }

            // ---- rich per-body overlays (outline-unify pivot, Item A) ------
            //
            // All three overlays anchor on the body's WORLD center of mass so a
            // compound (off-origin COM) body reads correctly. The COM is in the
            // same world*zoom+offset screen space as the outline above.
            const float     angle = static_cast<float>(world.GetAngle(world.HandleOf(i)));
            const glm::vec2 comW  = ComWorldF(wpos, angle, world.LocalCenterSlot(i));
            const glm::vec2 comS  = comW * zoom + off;

            // Velocity ray: COM -> COM + v * scale (DYNAMIC + awake only; a
            // resting/zero-velocity body draws nothing so the overlay stays clean).
            if (opts.drawVelocities && btype == BodyType::Dynamic && awake)
            {
                const Vec2  v   = world.VelSlot(i);
                const float vx  = static_cast<float>(v.x);
                const float vy  = static_cast<float>(v.y);
                const float spd = std::sqrt(vx * vx + vy * vy);
                if (spd > 1.0f)   // world units/s threshold -> suppress jitter
                {
                    const glm::vec2 tip =
                        comS + glm::vec2(vx, vy) * (opts.velocityScale * zoom);
                    batcher.Line(comS, tip, thick, kColVelocity);
                    DrawArrowHead(batcher, comS, tip, thick, kColVelocity);
                }
            }

            // Orientation tick: COM along local +x (rotated by the body angle),
            // so rotation is visible even on a rotation-invariant circle outline.
            if (opts.drawOrientations)
            {
                const glm::vec2 dir = Rotate2D(glm::vec2(1.0f, 0.0f), angle);
                const glm::vec2 tip = comS + dir * (opts.orientationTickLen * zoom);
                batcher.Line(comS, tip, thick, kColOrient);
            }

            // COM marker: a small axis-aligned cross at the world COM (dynamic
            // bodies; statics/kinematics have COM == origin and add no insight).
            if (opts.drawComMarkers && btype == BodyType::Dynamic)
            {
                const float r = opts.comMarkerSize * zoom;
                batcher.Line(glm::vec2(comS.x - r, comS.y),
                             glm::vec2(comS.x + r, comS.y), thick, kColCom);
                batcher.Line(glm::vec2(comS.x, comS.y - r),
                             glm::vec2(comS.x, comS.y + r), thick, kColCom);
            }
        }

        // ---- contact lines (port of PhysicsDebug.lua lines 75-82) ----------
        //
        // A magenta line links each begun pair's centers; a small disc at the
        // midpoint makes the contact pop even when the two centers are close
        // (the ForEachContact pull API exposes the pair, not the manifold point,
        // so the midpoint is the best available "where" marker).
        if (opts.drawContacts)
        {
            world.ForEachContact([&](std::uint32_t a, std::uint32_t b)
            {
                const glm::vec2 pa = ToScreen(world.PosSlot(a), off, zoom);
                const glm::vec2 pb = ToScreen(world.PosSlot(b), off, zoom);
                batcher.Line(pa, pb, thick, kColContact);
                batcher.Circle((pa + pb) * 0.5f, 3.0f * zoom, kColContact);
            });
        }

        // ---- Slice A: mover-broadphase DynamicTree (leaves + candidate pairs) -
        //
        // Each live leaf draws its TIGHT box (bright) inside its FAT box (dim) so
        // the MARGIN-grown coherent-motion envelope is visible. The leaf id is a
        // FIXTURE slot; we build an id->tight-center map while iterating so the
        // candidate-pair links can connect the two fixtures' AABB centers without
        // a second lookup pass. FixtureBroadphaseTree() is null for a non-Tree
        // mover broadphase -- guarded.
        if (opts.drawFixtureTree)
        {
            if (const Physics::DynamicTree* tree = world.FixtureBroadphaseTree())
            {
                // id -> tight AABB center (world space), filled during the leaf walk.
                std::unordered_map<std::uint32_t, glm::vec2> centers;
                tree->ForEachLeaf(
                    [&](std::uint32_t id, const Aabb2& tight, const Aabb2& fat)
                    {
                        DrawAabbOutline(batcher, fat,   off, zoom, thick, kColTreeFat);
                        DrawAabbOutline(batcher, tight, off, zoom, thick, kColTreeTight);
                        const glm::vec2 c(
                            (static_cast<float>(tight.min.x) + static_cast<float>(tight.max.x)) * 0.5f,
                            (static_cast<float>(tight.min.y) + static_cast<float>(tight.max.y)) * 0.5f);
                        centers.emplace(id, c);
                    });

                // Candidate-pair links: a teal line between the two fixtures' AABB
                // centers. Pairs() keys are FIXTURE slots (same id space as the
                // leaves), so the id->center map resolves both ends.
                std::vector<Physics::BroadphasePair> pairs;
                world.FixtureBroadphase().Pairs(pairs);
                for (const Physics::BroadphasePair& p : pairs)
                {
                    const auto ia = centers.find(p.a);
                    if (ia == centers.end()) continue;
                    const auto ib = centers.find(p.b);
                    if (ib == centers.end()) continue;
                    const glm::vec2 sa = ia->second * zoom + off;
                    const glm::vec2 sb = ib->second * zoom + off;
                    batcher.Line(sa, sb, thick, kColTreePair);
                }
            }
        }

        // ---- Slice A: static-body SpatialGrid (occupied cells) ---------------
        //
        // Outline each occupied cell. Cell world AABB:
        //   min = Origin + (cx, cy) * TileSize,  max = min + (TileSize, TileSize).
        if (opts.drawStaticGrid)
        {
            const Physics::SpatialGrid& grid = world.StaticGrid();
            const float ts = static_cast<float>(grid.TileSize());
            const Physics::Vec2 gorg = grid.Origin();
            grid.ForEachCell(
                [&](int cx, int cy, const std::vector<std::uint32_t>&)
                {
                    Aabb2 cell;
                    cell.min = Physics::Vec2(gorg.x + static_cast<float>(cx) * ts,
                                             gorg.y + static_cast<float>(cy) * ts);
                    cell.max = Physics::Vec2(cell.min.x + ts, cell.min.y + ts);
                    DrawAabbOutline(batcher, cell, off, zoom, thick, kColStaticGrid);
                });
        }

        // ---- Slice A: residency SpatialGrid (occupied cells) -----------------
        //
        // Same as the static grid in a distinct warm tint, so static (cool blue)
        // vs residency (warm amber) read differently when both are on.
        if (opts.drawResidencyGrid)
        {
            const Physics::SpatialGrid& grid = world.ResidencyGrid();
            const float ts = static_cast<float>(grid.TileSize());
            const Physics::Vec2 gorg = grid.Origin();
            grid.ForEachCell(
                [&](int cx, int cy, const std::vector<std::uint32_t>&)
                {
                    Aabb2 cell;
                    cell.min = Physics::Vec2(gorg.x + static_cast<float>(cx) * ts,
                                             gorg.y + static_cast<float>(cy) * ts);
                    cell.max = Physics::Vec2(cell.min.x + ts, cell.min.y + ts);
                    DrawAabbOutline(batcher, cell, off, zoom, thick, kColResidencyGrid);
                });
        }

        // ---- Slice A: contact manifolds (per-point disc + normal arrow) -------
        //
        // For each ContactConstraint point, the WORLD contact point is the body's
        // world COM + the point's anchor (anchors are body-center-relative; see
        // ContactConstraintPoint). bodyA is ALWAYS a real dynamic slot; bodyB may
        // be kInvalidSlot (a tile-span virtual fixture), so anchorA is the
        // reliable end -- we anchor the marker on body A. The normal points B->A;
        // we draw the arrow from the contact point along it. Colored by the
        // narrowphase kind that produced the manifold. ADDITIVE to drawContacts.
        if (opts.drawManifolds)
        {
            world.ForEachContactConstraint(
                [&](const Physics::ContactConstraint& cc)
                {
                    if (cc.bodyA == Physics::kInvalidSlot)
                        return; // defensive: A is always a real dynamic slot

                    const Physics::Vec2 posA   = world.PosSlot(cc.bodyA);
                    const float         angA    = static_cast<float>(
                        world.GetAngle(world.HandleOf(cc.bodyA)));
                    const glm::vec2 comA = ComWorldF(
                        posA, angA, world.LocalCenterSlot(cc.bodyA));

                    const glm::vec4 col = ManifoldColor(cc.kind);
                    const glm::vec2 nrm(static_cast<float>(cc.normal.x),
                                        static_cast<float>(cc.normal.y));

                    for (int pi = 0; pi < cc.pointCount; ++pi)
                    {
                        const auto& cp = cc.points[pi];
                        // World contact point: bodyA COM + anchorA.
                        const glm::vec2 wpt = comA
                            + glm::vec2(static_cast<float>(cp.anchorA.x),
                                        static_cast<float>(cp.anchorA.y));
                        const glm::vec2 spt = wpt * zoom + off;
                        batcher.Circle(spt, kManifoldPointPx, col);
                        // Normal arrow: contact point -> point + normal * len.
                        const glm::vec2 tip =
                            (wpt + nrm * kManifoldNormalLen) * zoom + off;
                        batcher.Line(spt, tip, thick, col);
                        DrawArrowHead(batcher, spt, tip, thick, col);
                    }
                });
        }
    }

} // namespace Arcane
