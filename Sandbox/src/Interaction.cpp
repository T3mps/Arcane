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

        // Spawn tint (a readable orange). The HUD (Task 8) picks shape/size/density
        // via Interaction::SpawnCfg(); the tint stays fixed so spawned bodies read
        // as "player-spawned" against the authored scene palette.
        constexpr glm::vec4 kSpawnTint{0.95f, 0.55f, 0.25f, 1.0f};
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

        // ---- ImGui MOUSE CAPTURE GUARD (click-through fix) -------------------------
        // When ImGui owns the mouse (the cursor is over a HUD widget), the host sets
        // input.wantCaptureMouse. A mouse-sourced world edit (spawn/grab/pan/drag/
        // wheel-zoom) must NOT fire then -- the click belongs to the UI, not the world
        // behind it. We model "captured" as "the mouse buttons read as released": no
        // press edge fires, so no spawn/grab/pan starts; an in-progress grab releases
        // (lmbNow goes false -> lmbRelease -> clears the grab), which is the safe
        // behavior if the cursor is dragged onto the HUD mid-drag. Edge state is still
        // recorded at the end so the next un-captured frame starts clean. (The keyboard
        // zoom below stays live -- it is keyboard-sourced, not mouse-captured.)
        const bool mouseCaptured = input.wantCaptureMouse;
        const std::uint8_t buttonsNow = mouseCaptured ? std::uint8_t{0}
                                                       : input.mouseButtons;

        const bool lmbNow  = (buttonsNow  & kLMB) != 0;
        const bool lmbPrev = (m_prevButtons & kLMB) != 0;
        const bool rmbNow  = (buttonsNow  & kRMB) != 0;
        const bool rmbPrev = (m_prevButtons & kRMB) != 0;

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

        // ---- POLYGON MODE: an LMB press adds a vertex (no spawn/grab) ---------------
        // While polygon mode is active a left-click in the world collects a vertex for
        // the in-progress polygon instead of spawning the default shape or grabbing a
        // body. The HUD commits the list via SpawnPolygon. Camera nav still works
        // (keyboard zoom + RMB pan ran above); only the LMB grab/spawn/drive is
        // replaced by vertex collection. Edge state is recorded so each click is a
        // discrete press (a held LMB does not auto-repeat vertices).
        if (m_polygonMode)
        {
            if (lmbPress)
                m_polygonPoints.push_back(cursorWorld);

            m_prevButtons   = buttonsNow;
            m_prevMouse     = mouseNow;
            m_havePrevMouse = true;
            return;   // LMB belongs to the polygon: no grab/spawn/drive this frame
        }

        // ---- GRAB / SPAWN on LMB press --------------------------------------------
        if (lmbPress)
        {
            const Phys::BodyHandle hit = PickBodyAt(world, cursorWorld);
            if (hit != Phys::kInvalidBody)
            {
                m_grabbed = hit;                  // grab the body under the cursor
                // Capture the click point in the body's LOCAL frame so the drag
                // pulls THAT point (off-center grabs rotate the body):
                //   localAnchor = R(-angle) * (clickWorld - origin).
                const Phys::Vec2 o = world.Position(hit);
                const Phys::Real a = world.GetAngle(hit);
                const Phys::Vec2 la = Phys::RotateVec(
                    -a, Phys::Vec2(cursorWorld.x - o.x, cursorWorld.y - o.y));
                m_grabLocalAnchor = glm::vec2(la.x, la.y);
            }
            else
            {
                // Empty space -> spawn the HUD-selected shape at the cursor world
                // point. Box uses the size as a half-extent; Circle as a radius.
                const float sz = (m_spawn.size > 0.0f) ? m_spawn.size : 1.0f;
                if (m_spawn.shape == SpawnShape::Circle)
                    SpawnCircle(reg, Astra::Entity{}, cursorWorld, sz,
                                Phys::BodyType::Dynamic, kSpawnTint, m_spawn.density);
                else
                    SpawnBox(reg, Astra::Entity{}, cursorWorld, glm::vec2(sz, sz),
                             Phys::BodyType::Dynamic, kSpawnTint, m_spawn.density);
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
                // ---- mouse-spring as a BOUNDED-force POINT pull at the grab anchor.
                // The drive targets the GRAB POINT (origin + R(angle)*localAnchor),
                // not the COM, so an off-center grab rotates the body. It is a
                // capped impulse (never a velocity override) applied BEFORE
                // world.Step, so the contact solver resolves it the same step -- a
                // dragged body stops against obstacles and imparts bounded momentum.
                const std::uint32_t si = m_grabbed.index;
                const Phys::Vec2 o = world.Position(m_grabbed);   // body origin
                const Phys::Real a = world.GetAngle(m_grabbed);

                const Phys::Vec2 wa = Phys::Vec2(o.x, o.y) + Phys::RotateVec(
                    a, Phys::Vec2(m_grabLocalAnchor.x, m_grabLocalAnchor.y));
                const glm::vec2 worldAnchor(wa.x, wa.y);

                // Lever from the COM to the grab point (the drag torques about COM).
                const Phys::Vec2 com = Phys::Vec2(o.x, o.y)
                    + Phys::RotateVec(a, world.GetLocalCenter(m_grabbed));
                const glm::vec2 r(worldAnchor.x - com.x, worldAnchor.y - com.y);

                // Critically-damped target velocity for the grab point (reach the
                // cursor in ~one step), clamped to kDragMaxSpeed.
                glm::vec2 desiredVel = (dt > 0.0f)
                    ? (cursorWorld - worldAnchor) / dt : glm::vec2(0.0f);
                const float speed = glm::length(desiredVel);
                if (speed > kDragMaxSpeed && speed > 0.0f)
                    desiredVel *= (kDragMaxSpeed / speed);

                // Grab-point velocity = vCom + omega x r (CrossWR convention).
                const Phys::Vec2 vc = world.Velocity(m_grabbed);
                const float omega   = world.AngVelSlot(si);
                const glm::vec2 anchorVel(vc.x - omega * r.y, vc.y + omega * r.x);

                // Point-constraint effective mass K (Box2D b2MouseJoint form), so
                // the impulse accounts for the body's rotational inertia (no spin
                // blow-up on a long-lever / small-inertia grab):
                //   K = [ invM + invI*r.y^2 ,  -invI*r.x*r.y      ]
                //       [ -invI*r.x*r.y     ,   invM + invI*r.x^2 ]
                // invM/invI are the solver's actual inverses -> a fixedRotation
                // body (invI == 0) reduces to a pure-linear pull.
                const float invM = world.InvMassSlot(si);
                const float invI = world.InvInertiaSlot(si);
                const glm::vec2 cdv = desiredVel - anchorVel;
                const float k11 = invM + invI * r.y * r.y;
                const float k12 = -invI * r.x * r.y;
                const float k22 = invM + invI * r.x * r.x;
                const float det = k11 * k22 - k12 * k12;
                glm::vec2 impulse(0.0f, 0.0f);
                if (det != 0.0f)
                {
                    const float invDet = 1.0f / det;
                    impulse.x = invDet * ( k22 * cdv.x - k12 * cdv.y);
                    impulse.y = invDet * (-k12 * cdv.x + k11 * cdv.y);
                }

                // Cap the LINEAR momentum (bounded force the solver can resist)...
                const float mass = world.GetBodyMass(m_grabbed);
                const float maxImpulse = mass * kDragMaxAccel * dt;
                float impLen = glm::length(impulse);
                if (impLen > maxImpulse && impLen > 0.0f)
                {
                    impulse *= (maxImpulse / impLen);
                    impLen = maxImpulse;
                }
                // ...and clamp the per-step ANGULAR velocity change so an off-center
                // grab turns SMOOTHLY (the linear cap alone does not bound omega).
                const float dOmega = invI * (r.x * impulse.y - r.y * impulse.x);
                const float adO = std::abs(dOmega);
                if (adO > kDragMaxAngVel && adO > 0.0f)
                    impulse *= (kDragMaxAngVel / adO);

                world.Wake(m_grabbed);
                world.ApplyImpulse(m_grabbed, Phys::Vec2(impulse.x, impulse.y),
                                   Phys::Vec2(worldAnchor.x, worldAnchor.y));
            }
        }

        // ---- THROW / RELEASE -------------------------------------------------------
        // On LMB release stop driving + clear the grab. We do NOT zero the velocity:
        // the body keeps whatever the last drag set, so it flies off (the throw).
        if (lmbRelease)
            m_grabbed = Phys::kInvalidBody;

        // ---- record edge-detection state for next frame ----------------------------
        // Store the CAPTURE-MASKED buttons (released while ImGui owns the mouse), so the
        // first un-captured frame after the cursor leaves the HUD sees a clean released->
        // pressed edge rather than a spurious "already held" state.
        m_prevButtons    = buttonsNow;
        m_prevMouse      = mouseNow;
        m_havePrevMouse = true;
    }

    bool Interaction::SpawnPolygon(Phys::PhysicsWorld& world)
    {
        // The factory needs 3..kMaxPolyVerts verts (it THROWS outside that range). A
        // user can hit Spawn early or amass a huge list; guard both bounds so the live
        // (render-driven) HUD path never throws -- treat an out-of-range list as a
        // no-op that keeps the in-progress points.
        if (m_polygonPoints.size() < 3 ||
            m_polygonPoints.size() > Phys::kMaxPolyVerts)
            return false;

        // The clicked points are WORLD-space. Author the body at their centroid so it
        // rotates about its center (mass is computed about the shape centroid), with
        // the verts expressed RELATIVE to that origin -- mirrors the WorldPolygonBox
        // pattern in Scenes.cpp (local verts + a separate body position).
        glm::vec2 centroid{0.0f, 0.0f};
        for (const glm::vec2 p : m_polygonPoints) centroid += p;
        centroid /= static_cast<float>(m_polygonPoints.size());

        std::vector<Phys::Vec2> local;
        local.reserve(m_polygonPoints.size());
        for (const glm::vec2 p : m_polygonPoints)
            local.emplace_back(Phys::Real(p.x - centroid.x), Phys::Real(p.y - centroid.y));

        Phys::BodyDef def;
        def.type        = Phys::BodyType::Dynamic;
        def.position    = Phys::Vec2(centroid.x, centroid.y);
        def.shape       = Phys::MakePolygon(local);   // normalizes winding + bakes normals
        def.density     = Phys::Real(1);
        def.friction    = Phys::Real(0.5);
        def.restitution = Phys::Real(0.05);
        world.AddBody(def);

        m_polygonPoints.clear();   // committed -> start a fresh polygon on the next click
        return true;
    }
}
