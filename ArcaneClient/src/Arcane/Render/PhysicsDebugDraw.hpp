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
// the boundary is one-way.  No SDL3 / graphics headers in the options struct
// itself; its settings blocks bring Settings.hpp (reflection) with them.

#include <Arcane/Base/Api.hpp>
#include <Arcane/Render/PhysicsDebugSettings.hpp>   // the debug.physics.* blocks PhysicsDebugDrawOptions inherits
#include <Arcane/Scene/ViewTransform.hpp>   // Affine2D (PhysicsDebugDrawOptions::view)

#include <Manifold2D/Physics/PhysicsTypes.hpp>   // BodyHandle -- optional<T> needs it complete

#include <glm/vec2.hpp>

#include <cstdint>
#include <optional>

// Physics types were lifted to the standalone Manifold2D library (Phase 2);
// forward-declare them in their own namespace (this header only needs the types
// by reference in the function signatures below).
namespace Manifold2D::Physics
{
    class PhysicsWorld;
    struct NarrowphaseTrace;
}

namespace Arcane
{
    class Batcher2D;
    // Render interpolation buffer (Epic 04.2, defined in Scene/SceneResources.hpp).
    // Only referenced here through a pointer (PhysicsDebugDrawOptions::interp), so
    // a forward declaration is sufficient -- keeps this header free of the
    // Astra/Scene include chain (see the boundary note above).
    struct PhysicsInterpBuffer;

    // Options for DrawPhysicsDebug.
    //
    // Settings arc S6-10: the overlay's look, toggles and palette are the
    // debug.physics.* cvars (PhysicsDebugSettings.hpp). The options block
    // inherits the four settings blocks, so a default-constructed block draws
    // with their defaults (the pre-sweep literals) and
    // MakePhysicsDebugDrawOptions() draws with the published values; a caller
    // may still override any inherited member for one call. Inherited:
    //   DebugPhysicsSettings      lineThickness (canvas px), contactMarkerSize
    //                             (m, through view.Length), velocityScale (s of
    //                             look-ahead), velocityMinSpeed (m/s; slower
    //                             bodies draw no ray), comMarkerSize (m),
    //                             orientationTickLen (m), manifoldNormalLength,
    //                             manifoldPointPx;
    //   DebugPhysicsDrawSettings  contacts (centre-to-centre line + midpoint
    //                             disc per touching pool contact), aabbs (each body's tight
    //                             SlotAabb), velocities (awake dynamic bodies),
    //                             comMarkers (dynamic bodies), orientations
    //                             (local +x tick, so circles show rotation);
    //   DebugPhysicsColorSettings the palette (per body type, island, overlay
    //                             and NarrowphaseKind);
    //   DebugPhysicsTraceSettings DrawNarrowphaseWorldOverlay's defaults.
    // NOT inherited: DebugPhysicsStyleSettings (debug.physics.style.*: arrow
    // heads, sleeping dim, emphasis floor, thickness scales, px radii); both
    // overlays read its published values directly.
    struct PhysicsDebugDrawOptions : DebugPhysicsSettings, DebugPhysicsDrawSettings,
                                     DebugPhysicsColorSettings, DebugPhysicsTraceSettings
    {
        // Camera transform applied to every emitted point + length: the
        // orthographic ViewTransform's Affine2D (F4 plan 1 T3) -- points go
        // through view.Point (per-axis scale, y NEGATIVE for +Y up on a y-down
        // canvas), lengths through view.Length. Every shape's corners are
        // computed in WORLD space and projected one by one; nothing rotates in
        // screen space by a world angle (a mirrored map would reverse it).
        // Fill from RenderContext2D::view.AsAffine2D(), skipping the overlay
        // when that is nullopt (a perspective view). Default: unit scale, zero
        // offset -- a caller that sets no camera draws at 1 px per metre, y down.
        Affine2D view{};

        // ---- Slice A broadphase + manifold overlays (default OFF) -----------
        //
        // These consume the read-only debug-visualization accessors on
        // PhysicsWorld (FixtureBroadphaseTree / StaticTree / ResidencyGrid /
        // ForEachContactConstraint).  All default off, so behavior is unchanged
        // until a caller (the Sandbox HUD) opts in.

        // Mover-broadphase DynamicTree: each live leaf's tight + fat AABB plus a
        // line between the AABB centers of every broadphase candidate pair. No-op
        // when the world's mover broadphase is not a DynamicTree (null tree).
        bool drawFixtureTree = false;

        // Static-body DynamicTree: a tinted outline of every static leaf's fat
        // box. Flag name retained (renaming ripples to the Sandbox HUD).
        bool drawStaticGrid = false;

        // Dynamic/kinematic residency SpatialGrid: occupied cells in a distinct
        // tint so static vs residency read differently.
        bool drawResidencyGrid = false;

        // Contact manifolds: for each ContactConstraint point, a disc at the
        // world contact point + a normal arrow, colored by NarrowphaseKind. This
        // is ADDITIVE to the legacy center-to-center `contacts` line; both can
        // be on at once.
        bool drawManifolds = false;

        // ---- render interpolation (Epic 04.2) -------------------------------
        // When `interp` is set (per-body previous-step poses from PhysicsSystem)
        // each body's outline / COM / orientation / velocity origin is drawn at
        // lerp(prev, current, alpha). Null -> current step pose (unchanged). A
        // per-body generation mismatch (recycled slot) falls back to current.
        // The per-body AABB (aabbs), contacts, and the broadphase overlays
        // (drawFixtureTree / drawStaticGrid / drawResidencyGrid / drawManifolds)
        // are NOT interpolated -- they stay at the current step by spec.
        const PhysicsInterpBuffer* interp = nullptr;
        float                      alpha  = 0.0f;   // RunLoop::Alpha() in [0,1)

        // ---- one-body filter (2026-09-11 physics wiring, spec s6.3) ---------
        // When set, ONLY this body's shape outline is drawn -- no contacts,
        // AABBs, velocity rays, COM crosses, orientation ticks or manifolds --
        // so the editor can outline the SELECTED entity's collider in Edit mode
        // without the whole-world overlay (the Unity collider gizmo). Null (the
        // default) is every existing caller: the whole world, every flag honoured.
        std::optional<Manifold2D::Physics::BodyHandle> onlyBody;
    };

    // A fresh options block holding the PUBLISHED debug.physics.* values (the
    // per-call members keep their defaults). Read it once per frame.
    [[nodiscard]] ARC_API PhysicsDebugDrawOptions MakePhysicsDebugDrawOptions();

    // Submit physics debug geometry to `batcher`.
    //
    // The caller is responsible for bracketing batcher.Begin() ..
    // batcher.Drain() around this call.  DrawPhysicsDebug() only calls the
    // primitive submission methods (Line, Circle, Rect); it opens and closes
    // no bracket itself. (This said Begin()/End() until ABI v15; End() records
    // nothing and no caller invokes it -- see Batcher2D::End's declaration.)
    //
    // For each alive body the shape outline is drawn colored by body type /
    // island:
    //   * Static      -> blue-ish  (COL_STATIC)
    //   * Sensor      -> yellow-ish (COL_SENSOR)
    //   * Kinematic   -> green-ish (COL_KINEMATIC)
    //   * Dynamic     -> hue-keyed by IslandRootOf(i) from a small palette;
    //                    sleeping dynamics are drawn at 35% brightness.
    //
    // If opts.contacts, a magenta line connects the centers of each
    // touching pool contact (ForEachContact) with a midpoint disc.
    // If opts.aabbs, a white outline is drawn for each body's tight AABB
    // (SlotAabb).
    //
    // Rich per-body overlays (each gated by its option flag):
    //   * velocities   -> a velocity ray (COM along linear velocity, arrow).
    //   * comMarkers   -> a small cross at each dynamic body's world COM.
    //   * orientations -> a short tick along the body's local +x (rotation).
    ARC_API void DrawPhysicsDebug(
        const Manifold2D::Physics::PhysicsWorld& world,
        Batcher2D& batcher,
        const PhysicsDebugDrawOptions& opts = {});

    // ---- Slice B: narrowphase-inspector WORLD overlay ----------------------------
    //
    // Draws the WORLD-SPACE internals of one recorded narrowphase trace (one of the
    // inspector SUBJECT's contacts): the two colliding shapes outlined (shapeA == the
    // SUBJECT fixture, highlighted distinctly), the SAT candidate axes (chosen axis
    // bold) drawn through the contact region, the support points, the final
    // representative normal as an arrow, and the manifold contact points. This is the
    // world half of the inspector; the Minkowski half draws into an OffscreenCanvas (the
    // Sandbox HUD). Submits ONLY Batcher2D primitives (Line/Circle) -- no parallel
    // ImDrawList path (homogenized-rendering mandate).
    //
    // The subject's overlay draws ONE call PER contact: the caller emphasizes the
    // SELECTED contact (emphasis 1.0, bold) and dims the others (emphasis < 1.0) so all
    // contacts read while the focused one stands out. `emphasis` scales alpha + a subtle
    // brightness; shapeA (the subject) is always drawn at a recognisable highlight.
    //
    // `view` is the SAME Affine2D DrawPhysicsDebug takes (PhysicsDebugDrawOptions::view),
    // so the overlay registers with the sprites.
    // `stepIndex` selects the per-iteration snapshot to emphasize for stepped kinds
    // (Epa/Mpr/SatPolygon); pass -1 (or for analytic kinds) to draw no per-step
    // emphasis. The caller brackets batcher.Begin()..Drain() (this only submits primitives).
    // An absent lineThickness / emphasis takes the published debug.physics.trace
    // value (1.5 / 1.0 by default); the colours, the normal length and the
    // debug.physics.style.* block always do.
    ARC_API void DrawNarrowphaseWorldOverlay(
        const Manifold2D::Physics::NarrowphaseTrace& trace,
        int stepIndex,
        Batcher2D& batcher,
        const Affine2D& view,
        std::optional<float> lineThickness = std::nullopt,
        std::optional<float> emphasis = std::nullopt);

} // namespace Arcane
