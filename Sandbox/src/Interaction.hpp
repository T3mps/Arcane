#pragma once

// Interaction: the Sandbox's raw mouse/keyboard interaction layer (Task 7).
//
// Turns a per-frame InputSnapshot (window-px cursor + button bits) into direct
// edits on the PhysicsWorld + the Sandbox Camera, with NO HUD (that is Task 8):
//
//   * LMB press on EMPTY space  -> SPAWN a dynamic box at the cursor world point
//                                  (an Astra entity; PhysicsSystem mints the body
//                                  on the next fixedUpdate, same as a scene body).
//   * LMB press on a BODY       -> GRAB it (OverlapShape pick). While held, a
//                                  mouse-spring drives the body's velocity toward
//                                  the cursor world point each frame.
//   * LMB release               -> THROW: stop driving, clear the grab. The body
//                                  keeps whatever velocity the last drag built
//                                  (momentum carries past release).
//   * RMB drag                  -> PAN: camera.offset += (cursorNow - cursorPrev)
//                                  in screen px (offset IS the screen translation).
//   * '=' / '-' keys            -> ZOOM in / out (option B: keyboard, no new input
//                                  plumbing -- InputSnapshot has no wheel field).
//                                  Zoom is clamped to kMinZoom so ScreenToWorld
//                                  (which divides by zoom) never hits 0 (a Task-6
//                                  carry-forward).
//
// DEPENDENCY-INJECTED: Tick takes the Registry, PhysicsWorld, Camera, InputSnapshot,
// and dt as parameters (no hidden globals), so the CPU interaction test drives it
// against a hand-built world. The plugin (SandboxApp) owns one Interaction and feeds
// it Runtime::Input() + its Camera + the live PhysicsWorld each fixed step.
//
// Edge guard: a grabbed body can be destroyed out from under us (scene switch wipes
// the registry + mints a fresh PhysicsWorld). The grab handle is validated with
// world.IsValid(handle) before every drive; a stale handle silently clears the grab.

#include "Camera.hpp"

#include <Arcane/Physics/PhysicsTypes.hpp>   // BodyHandle, kInvalidBody

#include <cstdint>
#include <vector>

#include <glm/vec2.hpp>

namespace Astra { class Registry; }

namespace Arcane { struct InputSnapshot; }
namespace Arcane::Physics { class PhysicsWorld; }

namespace Arcane::Sandbox
{
    class Interaction
    {
    public:
        // ---- tuning + input constants (public so the test asserts the contract) ----

        // Minimum zoom. ScreenToWorld divides by zoom, so zoom must never reach 0
        // (Task-6 carry-forward). Clamp: zoom = max(kMinZoom, zoom * factor).
        static constexpr float kMinZoom = 0.1f;
        // Maximum zoom (keeps the view from collapsing a world unit to absurd px).
        static constexpr float kMaxZoom = 8.0f;
        // Per-press multiplicative zoom step ('=' multiplies, '-' divides).
        static constexpr float kZoomStep = 1.04f;

        // SDL3 keycodes (ASCII codepoints) for the zoom keys. '=' is the unshifted
        // '+' key (no modifier needed); '-' is unshifted minus. Stored as raw
        // values to keep this header SDL-include-free.
        static constexpr std::uint32_t kZoomInKeycode  = 0x3Du;   // SDLK_EQUALS '='
        static constexpr std::uint32_t kZoomOutKeycode = 0x2Du;   // SDLK_MINUS  '-'

        // Mouse-spring: max speed (world units/s) the drag drives a grabbed body at,
        // so a far cursor jump does not launch the body at an explosive velocity.
        static constexpr float kDragMaxSpeed = 4000.0f;

        // Pick radius (world units) for the cursor query shape -- a small circle at
        // the cursor; the first dynamic body it overlaps is grabbed.
        static constexpr float kPickRadius = 4.0f;

        // ---- per-frame entry point -------------------------------------------------
        //
        // Advance the interaction one frame. `input` is THIS frame's snapshot; the
        // Interaction holds the previous frame's button mask + cursor so it can detect
        // press/release edges and pan deltas. `dt` is the frame's timestep (used to
        // convert the cursor->body offset into a drive velocity for the mouse-spring).
        // Camera mutations (pan/zoom) happen here so the caller can push the camera
        // AFTER Tick in the same frame.
        void Tick(Astra::Registry& reg, Arcane::Physics::PhysicsWorld& world,
                  Camera& camera, const Arcane::InputSnapshot& input, float dt);

        // Drop any active grab + reset edge state. Called by SandboxApp on a scene
        // switch/reset: the old PhysicsWorld is destroyed and a fresh one minted, so a
        // held grab handle from the old world must not be reused against the new one
        // (cross-world handle aliasing). Belt-and-suspenders with the per-tick
        // world.IsValid guard.
        void ClearGrab() noexcept
        {
            m_grabbed        = Arcane::Physics::kInvalidBody;
            m_prevButtons    = 0;
            m_havePrevMouse  = false;
        }

        // ---- introspection (test hooks; also handy for a future HUD) ---------------
        [[nodiscard]] bool IsGrabbing() const noexcept
        {
            return m_grabbed != Arcane::Physics::kInvalidBody;
        }
        [[nodiscard]] Arcane::Physics::BodyHandle GrabbedHandleForTest() const noexcept
        {
            return m_grabbed;
        }

    private:
        // Pick the first DYNAMIC body whose shape overlaps a tiny circle at the cursor
        // world point. Returns kInvalidBody when the cursor is over empty space (or only
        // over static/kinematic bodies, which are not grabbable). Uses OverlapShape.
        Arcane::Physics::BodyHandle PickBodyAt(Arcane::Physics::PhysicsWorld& world,
                                               glm::vec2 worldPt) const;

        // Previous-frame edge-detection state.
        std::uint8_t m_prevButtons = 0;
        glm::vec2    m_prevMouse{0.0f, 0.0f};
        bool         m_havePrevMouse = false;    // first frame has no valid prev cursor

        // The grabbed body (kInvalidBody = nothing grabbed).
        Arcane::Physics::BodyHandle m_grabbed = Arcane::Physics::kInvalidBody;

        // Pooled scratch for OverlapShape candidate handles (reused; no per-frame alloc).
        mutable std::vector<Arcane::Physics::BodyHandle> m_overlapScratch;
    };
}
