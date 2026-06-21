// Interaction implementation (Task 7). See Interaction.hpp for the behavior map.
//
// Picking uses PhysicsWorld::OverlapShape(shape, xf, out): a tiny query circle at
// the cursor world point is overlapped against every body; the first DYNAMIC hit is
// grabbed (statics/kinematics are scenery, not grabbable). The mouse-spring drives a
// grabbed body by SETTING its velocity to (cursorWorld - bodyPos)/dt, clamped to
// kDragMaxSpeed -- so it reaches the cursor in ~one step but never explodes on a big
// jump. On release we stop driving; the body keeps that velocity (the throw). Spawn
// reuses Scenes.cpp's SpawnBox (component-driven; PhysicsSystem mints the body next
// fixedUpdate). Pan adds the screen-space cursor delta to camera.offset; zoom
// multiplies/divides camera.zoom per '='/'-' frame, clamped to [kMinZoom,kMaxZoom].

#include "Interaction.hpp"
#include "Scenes.hpp"

#include <Arcane/Input/InputSnapshot.hpp>
#include <Arcane/Physics/PhysicsWorld.hpp>
#include <Arcane/Physics/Shapes.hpp>

#include <Astra/Registry/Registry.hpp>

#include <glm/geometric.hpp>   // glm::length

#include <algorithm>           // std::clamp
#include <cmath>

namespace Arcane::Sandbox
{
    namespace
    {
        namespace Phys = Arcane::Physics;

        constexpr std::uint8_t kLMB = 0x1;   // mouseButtons bit0
        constexpr std::uint8_t kRMB = 0x2;   // mouseButtons bit1

        // Default spawn box tint + half-extents (the HUD picks shape/size in Task 8;
        // Task 7 spawns a single readable dynamic box).
        constexpr glm::vec4 kSpawnTint{0.95f, 0.55f, 0.25f, 1.0f};
        constexpr glm::vec2 kSpawnHalf{22.0f, 22.0f};
    }

    Phys::BodyHandle Interaction::PickBodyAt(Phys::PhysicsWorld& world,
                                             glm::vec2 worldPt) const
    {
        // Tiny query circle at the cursor; OverlapShape returns every body it touches.
        const Phys::Shape query = Phys::MakeCircle(Phys::Real(kPickRadius));
        Phys::Transform xf;
        xf.position = Phys::Vec2(worldPt.x, worldPt.y);
        xf.rotation = Phys::Real(0);

        world.OverlapShape(query, xf, m_overlapScratch);

        // First DYNAMIC hit wins (index-ordered -> deterministic). Statics/kinematics
        // are scenery; sensors are not grabbed either.
        for (const Phys::BodyHandle h : m_overlapScratch)
        {
            if (!world.IsValid(h)) continue;
            if (world.GetType(h) != Phys::BodyType::Dynamic) continue;
            if (world.IsSensor(h)) continue;
            return h;
        }
        return Phys::kInvalidBody;
    }

    void Interaction::Tick(Astra::Registry& reg, Phys::PhysicsWorld& world,
                           Camera& camera, const Arcane::InputSnapshot& input, float dt)
    {
        const glm::vec2 mouseNow{input.mouseX, input.mouseY};
        const glm::vec2 cursorWorld = camera.ScreenToWorld(mouseNow);

        const bool lmbNow  = (input.mouseButtons & kLMB) != 0;
        const bool lmbPrev = (m_prevButtons      & kLMB) != 0;
        const bool rmbNow  = (input.mouseButtons & kRMB) != 0;
        const bool rmbPrev = (m_prevButtons      & kRMB) != 0;

        const bool lmbPress   = lmbNow && !lmbPrev;
        const bool lmbRelease = !lmbNow && lmbPrev;

        // ---- ZOOM (keyboard '='/'-'; option B -- no wheel field on InputSnapshot) --
        // Multiplicative so each held frame zooms a constant ratio. Clamp to a
        // positive minimum so ScreenToWorld (divides by zoom) is never unsafe.
        if (input.KeycodeDown(kZoomInKeycode))
            camera.zoom = std::clamp(camera.zoom * kZoomStep, kMinZoom, kMaxZoom);
        if (input.KeycodeDown(kZoomOutKeycode))
            camera.zoom = std::clamp(camera.zoom / kZoomStep, kMinZoom, kMaxZoom);

        // ---- PAN (RMB drag) -- offset += screen-space cursor delta -----------------
        // Only when RMB was held across BOTH frames (so we have a valid prev cursor and
        // are not jumping on the press edge). offset is the screen translation, so the
        // raw px delta is the correct shift (the view follows the drag).
        if (rmbNow && rmbPrev && m_havePrevMouse)
            camera.offset += (mouseNow - m_prevMouse);

        // ---- GRAB / SPAWN on LMB press --------------------------------------------
        if (lmbPress)
        {
            const Phys::BodyHandle hit = PickBodyAt(world, cursorWorld);
            if (hit != Phys::kInvalidBody)
            {
                m_grabbed = hit;                  // grab the body under the cursor
            }
            else
            {
                // Empty space -> spawn a dynamic box at the cursor world point.
                SpawnBox(reg, Astra::Entity{}, cursorWorld, kSpawnHalf,
                         Phys::BodyType::Dynamic, kSpawnTint);
            }
        }

        // ---- DRIVE the grabbed body (mouse-spring) while LMB held ------------------
        if (m_grabbed != Phys::kInvalidBody && lmbNow)
        {
            // The grab may have gone stale (scene switch wiped the world). Validate.
            if (!world.IsValid(m_grabbed))
            {
                m_grabbed = Phys::kInvalidBody;
            }
            else
            {
                const Phys::Vec2 bp = world.Position(m_grabbed);
                glm::vec2 toCursor = cursorWorld - glm::vec2(bp.x, bp.y);
                // Velocity that reaches the cursor in ~one step, clamped to a sane max.
                glm::vec2 drive = (dt > 0.0f) ? (toCursor / dt) : glm::vec2(0.0f);
                const float speed = glm::length(drive);
                if (speed > kDragMaxSpeed && speed > 0.0f)
                    drive *= (kDragMaxSpeed / speed);
                world.Wake(m_grabbed);
                world.SetVelocity(m_grabbed, Phys::Vec2(drive.x, drive.y));
            }
        }

        // ---- THROW / RELEASE -------------------------------------------------------
        // On LMB release stop driving + clear the grab. We do NOT zero the velocity:
        // the body keeps whatever the last drag set, so it flies off (the throw).
        if (lmbRelease)
            m_grabbed = Phys::kInvalidBody;

        // ---- record edge-detection state for next frame ----------------------------
        m_prevButtons    = input.mouseButtons;
        m_prevMouse      = mouseNow;
        m_havePrevMouse = true;
    }
}
