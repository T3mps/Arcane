#pragma once
#include <Arcane/Modules/ArcaneRenderApi.hpp>

// Render module: physics debug-draw overlay (M6, Task P3.6).
//
// ENGINE-INTERNAL, NOT GAME API: the engine's hosts (editor, runtime) draw
// the physics debug overlay; game code never includes this header. Its
// options and functions name Manifold2D types on purpose (flat-API spec
// 2026-10-10, FA10), so it is not in the spelling guard's game-facing set.
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

#include <Arcane/Modules/ArcaneRenderApi.hpp>
#include <Arcane/Render/PhysicsDebugSettings.hpp>   // the debug.physics.* blocks PhysicsDebugDrawOptions inherits
#include <Arcane/Scene/ViewTransform.hpp>   // Affine2D (PhysicsDebugDrawOptions::view)

#include <Arcane/Scene/PhysicsDebugGeometry.hpp>

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
    struct PhysicsInterpBuffer2D;

    // Options for DrawPhysicsDebug.
    //
    // Settings arc S6-10: the overlay's look, toggles and palette are the
    // debug.physics.* cvars (PhysicsDebugSettings.hpp). The options block
    // inherits the four settings blocks, so a default-constructed block draws
    // with their defaults (the pre-sweep literals) and
    // MakePhysicsDebugDrawOptions() draws with the published values; a caller
    // may still override any inherited member for one call. Inherited:
    //   PhysicsDebugSettings2D    lineThickness (canvas px), contactMarkerSize
    //                             (m, through view.Length), velocityScale (s of
    //                             look-ahead), velocityMinSpeed (m/s; slower
    //                             bodies draw no ray), comMarkerSize (m),
    //                             orientationTickLen (m), manifoldNormalLength,
    //                             manifoldPointPx;
    //   PhysicsDebugDrawSettings2D contacts (centre-to-centre line + midpoint
    //                             disc per touching pool contact), aabbs (each body's tight
    //                             SlotAabb), velocities (awake dynamic bodies),
    //                             comMarkers (dynamic bodies), orientations
    //                             (local +x tick, so circles show rotation);
    //   PhysicsDebugColorSettings2D the palette (per body type, island, overlay
    //                             and NarrowphaseKind);
    //   PhysicsDebugTraceSettings2D DrawNarrowphaseWorldOverlay's defaults.
    // NOT inherited: PhysicsDebugStyleSettings2D (debug.physics.style.*: arrow
    // heads, sleeping dim, emphasis floor, thickness scales, px radii); both
    // overlays read its published values directly.
    // A fresh options block holding the PUBLISHED debug.physics.* values (the
    // per-call members keep their defaults). Read it once per frame.
    [[nodiscard]] ARC_RENDER_API PhysicsDebugDrawOptions2D MakePhysicsDebugDrawOptions();

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
    ARC_RENDER_API void DrawPhysicsDebug(
        const Manifold2D::Physics::PhysicsWorld& world,
        Batcher2D& batcher,
        const PhysicsDebugDrawOptions2D& opts = {});

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
    ARC_RENDER_API void DrawNarrowphaseWorldOverlay(
        const Manifold2D::Physics::NarrowphaseTrace& trace,
        int stepIndex,
        Batcher2D& batcher,
        const Affine2D& view,
        std::optional<float> lineThickness = std::nullopt,
        std::optional<float> emphasis = std::nullopt);

} // namespace Arcane
