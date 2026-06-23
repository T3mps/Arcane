#pragma once

// Render module: physics debug-draw overlay (M6, Task P3.6).
//
// Ports Client/src/physics/PhysicsDebug.lua into the Arcane.dll render side,
// consuming the PULL API added to PhysicsWorld (ForEachContact /
// IslandRootOf) and submitting primitives to the Batcher2D.
//
// MODERNIZATION over the Lua: dynamic bodies are colored BY ISLAND rather
// than a single kinematic color.  IslandRootOf(i) keys into a small hash-to-
// hue palette so every body in the same dynamic island shares a color; bodies
// in different islands get distinct colors.  Sleeping dynamics are drawn dim
// (x 0.35 multiplier, matching the Lua).  Static/kinematic/sensor bodies keep
// the Lua's type-based tints.
//
// PRESENTATION BOUNDARY: this file lives in Arcane.dll (Render/).  It includes
// PhysicsWorld.hpp (Core) and Batcher2D.hpp.  Core never includes Render --
// the boundary is one-way.  No Astra / SDL3 / NVRHI headers in the options
// struct itself; only Batcher2D.hpp is included here.

#include <Arcane/Base/Api.hpp>

#include <glm/vec2.hpp>

namespace Arcane
{
    class Batcher2D;

    namespace Physics
    {
        class PhysicsWorld;
    }

    // Options for DrawPhysicsDebug.
    struct PhysicsDebugDrawOptions
    {
        // Camera transform applied to every emitted point + length: screen =
        // world * zoom + offset.  This is the CANONICAL form shared with
        // RenderContext2D / RenderSubmissionSystem / Sandbox::Camera::WorldToScreen,
        // so the overlay lines up with the sprites under pan + zoom.  Defaults
        // (offset (0,0), zoom 1) are the identity transform -- every existing
        // caller that does not set a camera is unchanged.
        glm::vec2 cameraOffset{ 0.0f, 0.0f };  // screen-space translation (canvas px)
        float     zoom = 1.0f;                  // world->screen scale (1 == 1:1)

        // Thickness (canvas pixels) for Line primitives.
        float lineThickness = 1.0f;

        // Draw a line between the centers of each active contact pair
        // (begun == true in the ContactManager).  Magenta, like COL_CONTACT
        // in PhysicsDebug.lua.  A small disc marks the segment midpoint so the
        // contact reads clearly even when the two body centers are close.
        bool drawContacts = true;

        // Outline each body's world-space AABB (SlotAabb).  Off by default;
        // useful when debugging the broadphase.
        bool drawAabbs = false;

        // ---- rich per-body overlays (Sandbox outline-unify pivot, Item A) ---
        //
        // DrawPhysicsDebug is the SINGLE canonical Sandbox renderer now (bodies
        // are no longer drawn as filled SpriteRenderer quads), so it grew richer
        // debug geometry.  Each overlay is GENERIC (every shape) and gated by a
        // flag so the HUD can toggle it; defaults ON for the velocity/COM/
        // orientation set keep the showcase informative out of the box while a
        // headless caller that constructs default options still gets them.

        // Velocity vector: a line from each awake DYNAMIC body's world COM along
        // its linear velocity (scaled by `velocityScale`), tipped with a small
        // arrow head.  Suppressed for bodies at rest (|v| ~ 0) to avoid clutter.
        bool  drawVelocities = true;
        // Seconds of look-ahead for the velocity ray length (world = v * scale,
        // then * zoom).  0.15 s reads well at the sandbox scale.
        float velocityScale  = 0.15f;

        // Center-of-mass marker: a small cross (two short lines) at each DYNAMIC
        // body's world COM.  Makes the off-origin COM of compound bodies visible.
        bool  drawComMarkers = true;
        // Half-length (canvas px, pre-zoom) of each COM cross arm.
        float comMarkerSize  = 5.0f;

        // Orientation tick: a short line from each body's COM along its local +x
        // axis, so rotation is visible even on a circle (whose outline is
        // rotation-invariant).  Length is `orientationTickLen` (world units).
        bool  drawOrientations = true;
        float orientationTickLen = 18.0f;

        // ---- Slice A broadphase + manifold overlays (default OFF) -----------
        //
        // These consume the read-only debug-visualization accessors on
        // PhysicsWorld (FixtureBroadphaseTree / StaticGrid / ResidencyGrid /
        // ForEachContactConstraint).  All default off, so behavior is unchanged
        // until a caller (the Sandbox HUD) opts in.

        // Mover-broadphase DynamicTree: each live leaf's tight + fat AABB plus a
        // line between the AABB centers of every broadphase candidate pair. No-op
        // when the world's mover broadphase is not a DynamicTree (null tree).
        bool drawFixtureTree = false;

        // Static-body SpatialGrid: a tinted outline of every occupied cell.
        bool drawStaticGrid = false;

        // Dynamic/kinematic residency SpatialGrid: occupied cells in a distinct
        // tint so static vs residency read differently.
        bool drawResidencyGrid = false;

        // Contact manifolds: for each ContactConstraint point, a disc at the
        // world contact point + a normal arrow, colored by NarrowphaseKind. This
        // is ADDITIVE to the legacy center-to-center `drawContacts` line; both can
        // be on at once.
        bool drawManifolds = false;
    };

    // Submit physics debug geometry to `batcher`.
    //
    // The caller is responsible for bracketing batcher.Begin() / batcher.End()
    // around this call.  DrawPhysicsDebug() only calls the primitive submission
    // methods (Line, Circle, Rect); it does not call Begin/End itself.
    //
    // For each alive body the shape outline is drawn colored by body type /
    // island:
    //   * Static      -> blue-ish  (COL_STATIC)
    //   * Sensor      -> yellow-ish (COL_SENSOR)
    //   * Kinematic   -> green-ish (COL_KINEMATIC)
    //   * Dynamic     -> hue-keyed by IslandRootOf(i) from a small palette;
    //                    sleeping dynamics are drawn at 35% brightness.
    //
    // If opts.drawContacts, a magenta line connects the centers of each
    // currently-begun contact pair (ForEachContact) with a midpoint disc.
    // If opts.drawAabbs, a white outline is drawn for each body's tight AABB
    // (SlotAabb).
    //
    // Rich per-body overlays (each gated by its option flag):
    //   * drawVelocities   -> a velocity ray (COM along linear velocity, arrow).
    //   * drawComMarkers   -> a small cross at each dynamic body's world COM.
    //   * drawOrientations -> a short tick along the body's local +x (rotation).
    ARCANE_API void DrawPhysicsDebug(
        const Arcane::Physics::PhysicsWorld& world,
        Batcher2D& batcher,
        const PhysicsDebugDrawOptions& opts = {});

} // namespace Arcane
