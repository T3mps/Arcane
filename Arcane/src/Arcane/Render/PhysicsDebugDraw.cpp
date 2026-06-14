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

#include <Arcane/Physics/PhysicsTypes.hpp>
#include <Arcane/Physics/PhysicsWorld.hpp>
#include <Arcane/Physics/Shapes.hpp>
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
        static const glm::vec4 kIslandPalette[8] =
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

        // Apply camera offset to a world-space position -> canvas position.
        inline glm::vec2 ToScreen(const Physics::Vec2& wpos,
                                  const glm::vec2& offset)
        {
            return glm::vec2(static_cast<float>(wpos.x) + offset.x,
                             static_cast<float>(wpos.y) + offset.y);
        }

        // Draw an AABB outline using four lines.
        inline void DrawAabbOutline(Batcher2D& b,
                                    const Physics::Aabb& aabb,
                                    const glm::vec2& off,
                                    float thickness,
                                    const glm::vec4& color)
        {
            const glm::vec2 mn = glm::vec2(static_cast<float>(aabb.min.x),
                                           static_cast<float>(aabb.min.y)) + off;
            const glm::vec2 mx = glm::vec2(static_cast<float>(aabb.max.x),
                                           static_cast<float>(aabb.max.y)) + off;
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
            const glm::vec2 spos   = ToScreen(wpos, off);

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
                    batcher.Circle(spos, static_cast<float>(s.radius), col);
                    break;
                }

                case ShapeKind::Capsule:
                {
                    // Lua: two end circles + two side lines.
                    // Capsule: segment (-halfLen,0)-(+halfLen,0) inflated by radius.
                    const float hl = static_cast<float>(s.halfLen);
                    const float r  = static_cast<float>(s.radius);
                    const glm::vec2 left (spos.x - hl, spos.y);
                    const glm::vec2 right(spos.x + hl, spos.y);
                    batcher.Circle(left,  r, col);
                    batcher.Circle(right, r, col);
                    batcher.Line(glm::vec2(spos.x - hl, spos.y - r),
                                 glm::vec2(spos.x + hl, spos.y - r),
                                 thick, col);
                    batcher.Line(glm::vec2(spos.x - hl, spos.y + r),
                                 glm::vec2(spos.x + hl, spos.y + r),
                                 thick, col);
                    break;
                }

                case ShapeKind::Aabb:
                {
                    // Lua: lg.rectangle("line", x-hw, y-hh, hw*2, hh*2)
                    // Four lines forming the box outline.
                    const float hw = static_cast<float>(s.halfW);
                    const float hh = static_cast<float>(s.halfH);
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
                        const glm::vec2 a = spos + Rotate2D(
                            glm::vec2(static_cast<float>(va.x),
                                      static_cast<float>(va.y)), angle);
                        const glm::vec2 b2 = spos + Rotate2D(
                            glm::vec2(static_cast<float>(vb.x),
                                      static_cast<float>(vb.y)), angle);
                        batcher.Line(a, b2, thick, col);
                    }
                    break;
                }
            }

            // ---- optional AABB outline (opts.drawAabbs) --------------------
            if (opts.drawAabbs)
            {
                DrawAabbOutline(batcher, world.SlotAabb(i), off, thick, kColAabb);
            }
        }

        // ---- contact lines (port of PhysicsDebug.lua lines 75-82) ----------
        if (opts.drawContacts)
        {
            world.ForEachContact([&](std::uint32_t a, std::uint32_t b)
            {
                const glm::vec2 pa = ToScreen(world.PosSlot(a), off);
                const glm::vec2 pb = ToScreen(world.PosSlot(b), off);
                batcher.Line(pa, pb, thick, kColContact);
            });
        }
    }

} // namespace Arcane
