#pragma once

// debug.physics.* (settings arc S6-10; inventory Part 2 "Physics debug draw"):
// the physics debug overlay's look, toggles and palette.
// - Game Dev, per-project preference, Live. PhysicsDebugDrawOptions inherits
//   all four blocks, so a default-constructed options block carries these
//   defaults and MakePhysicsDebugDrawOptions() carries the published values.
// - The defaults are the pre-sweep literals of PhysicsDebugDraw.hpp/.cpp, bit
//   for bit; SweepPhysicsDebugTest pins them.
// - Colours are linear RGBA (the batcher's glm::vec4, may be HDR).
// - debug.physics.style.* (arrow heads, dim, emphasis floor, px radii) stays
//   in PhysicsDebugDraw.cpp: it is DERIVED px styling (S6-43).

#include <Arcane/Config/Settings.hpp>

namespace Arcane
{
    struct DebugPhysicsSettings
    {
        float lineThickness        = 1.0f;    // canvas px, Line primitives
        float contactMarkerSize    = 0.03f;   // m, contact-midpoint disc radius
        float velocityScale        = 0.15f;   // s of look-ahead for the velocity ray
        float velocityMinSpeed     = 0.05f;   // m/s; the MKS sleep threshold (was velocityRayMinSpeed)
        float comMarkerSize        = 0.05f;   // m, COM cross half-arm
        float orientationTickLen   = 0.18f;   // m
        float manifoldNormalLength = 20.0f;   // "world units": a pre-metres leftover, converted as is
        float manifoldPointPx      = 3.0f;    // canvas px
    };

    struct DebugPhysicsDrawSettings
    {
        bool contacts     = true;
        bool aabbs        = false;
        bool velocities   = true;
        bool comMarkers   = true;
        bool orientations = true;
    };

    struct DebugPhysicsColorSettings
    {
        CVarColor kinematic     { 0.2f, 1.0f, 0.4f, 1.0f };
        CVarColor staticBody    { 0.4f, 0.7f, 1.0f, 1.0f };      // cvar debug.physics.color.static
        CVarColor sensor        { 1.0f, 0.9f, 0.2f, 0.9f };
        CVarColor contact       { 1.0f, 0.2f, 1.0f, 1.0f };
        CVarColor aabb          { 1.0f, 1.0f, 1.0f, 0.4f };
        CVarColor velocity      { 0.20f, 1.00f, 0.55f, 1.0f };
        CVarColor com           { 1.00f, 1.00f, 1.00f, 1.0f };
        CVarColor orient        { 1.00f, 0.55f, 0.15f, 1.0f };
        CVarColor treeTight     { 0.30f, 0.90f, 1.00f, 0.85f };
        CVarColor treeFat       { 0.30f, 0.90f, 1.00f, 0.25f };
        CVarColor treePair      { 0.20f, 1.00f, 0.90f, 0.80f };
        CVarColor staticGrid    { 0.35f, 0.55f, 1.00f, 0.35f };
        CVarColor residencyGrid { 1.00f, 0.70f, 0.20f, 0.35f };
        CVarColor traceShapeB   { 0.40f, 0.70f, 1.00f, 1.0f };
        CVarColor traceAxis     { 0.55f, 0.55f, 0.60f, 0.6f };
        CVarColor traceAxisHi   { 1.00f, 0.85f, 0.20f, 1.0f };
        CVarColor traceNormal   { 1.00f, 0.25f, 1.00f, 1.0f };
        CVarColor tracePoint    { 1.00f, 1.00f, 1.00f, 1.0f };
        CVarColor subject       { 1.00f, 0.95f, 0.35f, 1.0f };
        // Dynamic bodies are tinted by island: islandRoot % 8 picks one.
        CVarColor island0       { 1.0f, 0.35f, 0.35f, 1.0f };
        CVarColor island1       { 1.0f, 0.65f, 0.15f, 1.0f };
        CVarColor island2       { 1.0f, 1.00f, 0.20f, 1.0f };
        CVarColor island3       { 0.2f, 0.95f, 0.35f, 1.0f };
        CVarColor island4       { 0.2f, 0.80f, 1.00f, 1.0f };
        CVarColor island5       { 0.5f, 0.35f, 1.00f, 1.0f };
        CVarColor island6       { 1.0f, 0.30f, 0.90f, 1.0f };
        CVarColor island7       { 0.85f, 0.85f, 0.85f, 1.0f };
        // Manifold points are tinted by NarrowphaseKind ordinal (0 Separated, 1
        // CircleCircle, 2 CircleVsPolygon, 3 Capsule, 4 SatPolygon, 5 Epa, 6 Mpr);
        // an unknown kind takes narrowphase0.
        CVarColor narrowphase0  { 0.70f, 0.70f, 0.70f, 1.0f };
        CVarColor narrowphase1  { 1.00f, 0.30f, 0.30f, 1.0f };
        CVarColor narrowphase2  { 1.00f, 0.65f, 0.15f, 1.0f };
        CVarColor narrowphase3  { 1.00f, 1.00f, 0.25f, 1.0f };
        CVarColor narrowphase4  { 0.30f, 1.00f, 0.45f, 1.0f };
        CVarColor narrowphase5  { 0.40f, 0.70f, 1.00f, 1.0f };
        CVarColor narrowphase6  { 0.80f, 0.45f, 1.00f, 1.0f };
    };

    // The narrowphase-inspector world overlay (DrawNarrowphaseWorldOverlay).
    struct DebugPhysicsTraceSettings
    {
        float traceLineThickness = 1.5f;   // canvas px; cvar debug.physics.trace.lineThickness (the
                                           // member name keeps it apart from the inherited overlay lineThickness)
        float emphasis           = 1.0f;    // alpha scale when the caller passes none (1 = the selected contact)
        float normalLength       = 28.0f;   // "world units": a pre-metres leftover, converted as is
    };

    ARC_REFLECT_TYPE(DebugPhysicsSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "debug.physics", SettingScope::PreferencesProject, ApplyMode::Live, Audience::Game)
        ARC_REFLECT_TYPE_ATTR(Flags, CVarFlags::Dev)
        ARC_REFLECT_FIELD(DebugPhysicsSettings, lineThickness)
            ARC_REFLECT_ATTR(Range, 0.5, 8.0)
            ARC_REFLECT_ATTR(Tooltip, "Line thickness (px) of the physics debug overlay.")
        ARC_REFLECT_FIELD(DebugPhysicsSettings, contactMarkerSize)
            ARC_REFLECT_ATTR(Range, 0.001, 1.0)
            ARC_REFLECT_ATTR(Tooltip, "Radius (m) of the disc marking each contact pair's midpoint.")
        ARC_REFLECT_FIELD(DebugPhysicsSettings, velocityScale)
            ARC_REFLECT_ATTR(Range, 0.0, 5.0)
            ARC_REFLECT_ATTR(Tooltip, "Seconds of look-ahead drawn by the velocity ray (length = speed x this).")
        ARC_REFLECT_FIELD(DebugPhysicsSettings, velocityMinSpeed)
            ARC_REFLECT_ATTR(Range, 0.0, 10.0)
            ARC_REFLECT_ATTR(Tooltip, "Slowest speed (m/s) that still draws a velocity ray; slower is treated as jitter.")
        ARC_REFLECT_FIELD(DebugPhysicsSettings, comMarkerSize)
            ARC_REFLECT_ATTR(Range, 0.001, 1.0)
            ARC_REFLECT_ATTR(Tooltip, "Half-length (m) of each arm of the centre-of-mass cross.")
        ARC_REFLECT_FIELD(DebugPhysicsSettings, orientationTickLen)
            ARC_REFLECT_ATTR(Range, 0.01, 5.0)
            ARC_REFLECT_ATTR(Tooltip, "Length (m) of the tick along each body's local +X.")
        ARC_REFLECT_FIELD(DebugPhysicsSettings, manifoldNormalLength)
            ARC_REFLECT_ATTR(Range, 0.01, 100.0)
            ARC_REFLECT_ATTR(Tooltip, "Length (world units) of each manifold point's normal arrow.")
        ARC_REFLECT_FIELD(DebugPhysicsSettings, manifoldPointPx)
            ARC_REFLECT_ATTR(Range, 1.0, 16.0)
            ARC_REFLECT_ATTR(Tooltip, "Radius (px) of the disc drawn at each manifold contact point.")
    ARC_END_REFLECT_TYPE()

    ARC_REFLECT_TYPE(DebugPhysicsDrawSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "debug.physics.draw", SettingScope::PreferencesProject, ApplyMode::Live, Audience::Game)
        ARC_REFLECT_TYPE_ATTR(Flags, CVarFlags::Dev)
        ARC_REFLECT_FIELD(DebugPhysicsDrawSettings, contacts)
            ARC_REFLECT_ATTR(Tooltip, "Draw a line between the centres of every touching body pair.")
        ARC_REFLECT_FIELD(DebugPhysicsDrawSettings, aabbs)
            ARC_REFLECT_ATTR(Tooltip, "Outline every body's bounding box (broadphase debugging).")
        ARC_REFLECT_FIELD(DebugPhysicsDrawSettings, velocities)
            ARC_REFLECT_ATTR(Tooltip, "Draw a velocity ray from each awake dynamic body.")
        ARC_REFLECT_FIELD(DebugPhysicsDrawSettings, comMarkers)
            ARC_REFLECT_ATTR(Tooltip, "Mark each dynamic body's centre of mass with a cross.")
        ARC_REFLECT_FIELD(DebugPhysicsDrawSettings, orientations)
            ARC_REFLECT_ATTR(Tooltip, "Draw a tick along each body's local +X so rotation shows, even on circles.")
    ARC_END_REFLECT_TYPE()

    ARC_REFLECT_TYPE(DebugPhysicsColorSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "debug.physics.color", SettingScope::PreferencesProject, ApplyMode::Live, Audience::Game)
        ARC_REFLECT_TYPE_ATTR(Flags, CVarFlags::Dev)
        ARC_REFLECT_FIELD(DebugPhysicsColorSettings, kinematic)     ARC_REFLECT_ATTR(Tooltip, "Outline colour of kinematic bodies.")
        ARC_REFLECT_FIELD_NAMED(DebugPhysicsColorSettings, staticBody, "static") ARC_REFLECT_ATTR(Tooltip, "Outline colour of static bodies.")
        ARC_REFLECT_FIELD(DebugPhysicsColorSettings, sensor)        ARC_REFLECT_ATTR(Tooltip, "Outline colour of sensor bodies.")
        ARC_REFLECT_FIELD(DebugPhysicsColorSettings, contact)       ARC_REFLECT_ATTR(Tooltip, "Colour of the contact-pair lines and midpoint discs.")
        ARC_REFLECT_FIELD(DebugPhysicsColorSettings, aabb)          ARC_REFLECT_ATTR(Tooltip, "Colour of the per-body bounding-box outlines.")
        ARC_REFLECT_FIELD(DebugPhysicsColorSettings, velocity)      ARC_REFLECT_ATTR(Tooltip, "Colour of the velocity rays.")
        ARC_REFLECT_FIELD(DebugPhysicsColorSettings, com)           ARC_REFLECT_ATTR(Tooltip, "Colour of the centre-of-mass crosses.")
        ARC_REFLECT_FIELD(DebugPhysicsColorSettings, orient)        ARC_REFLECT_ATTR(Tooltip, "Colour of the orientation ticks.")
        ARC_REFLECT_FIELD(DebugPhysicsColorSettings, treeTight)     ARC_REFLECT_ATTR(Tooltip, "Colour of the broadphase tree's tight leaf boxes.")
        ARC_REFLECT_FIELD(DebugPhysicsColorSettings, treeFat)       ARC_REFLECT_ATTR(Tooltip, "Colour of the broadphase tree's fat (enlarged) leaf boxes.")
        ARC_REFLECT_FIELD(DebugPhysicsColorSettings, treePair)      ARC_REFLECT_ATTR(Tooltip, "Colour of the links between broadphase candidate pairs.")
        ARC_REFLECT_FIELD(DebugPhysicsColorSettings, staticGrid)    ARC_REFLECT_ATTR(Tooltip, "Colour of the static-body tree overlay.")
        ARC_REFLECT_FIELD(DebugPhysicsColorSettings, residencyGrid) ARC_REFLECT_ATTR(Tooltip, "Colour of the occupied residency-grid cells.")
        ARC_REFLECT_FIELD(DebugPhysicsColorSettings, traceShapeB)   ARC_REFLECT_ATTR(Tooltip, "Narrowphase inspector: outline colour of the contact partner.")
        ARC_REFLECT_FIELD(DebugPhysicsColorSettings, traceAxis)     ARC_REFLECT_ATTR(Tooltip, "Narrowphase inspector: colour of the candidate separating axes.")
        ARC_REFLECT_FIELD(DebugPhysicsColorSettings, traceAxisHi)   ARC_REFLECT_ATTR(Tooltip, "Narrowphase inspector: colour of the chosen axis.")
        ARC_REFLECT_FIELD(DebugPhysicsColorSettings, traceNormal)   ARC_REFLECT_ATTR(Tooltip, "Narrowphase inspector: colour of the contact normal arrow.")
        ARC_REFLECT_FIELD(DebugPhysicsColorSettings, tracePoint)    ARC_REFLECT_ATTR(Tooltip, "Narrowphase inspector: colour of the support and contact points.")
        ARC_REFLECT_FIELD(DebugPhysicsColorSettings, subject)       ARC_REFLECT_ATTR(Tooltip, "Narrowphase inspector: highlight colour of the inspected shape.")
        ARC_REFLECT_FIELD(DebugPhysicsColorSettings, island0)       ARC_REFLECT_ATTR(Tooltip, "Island palette, entry 0 (dynamic bodies are tinted by island).")
        ARC_REFLECT_FIELD(DebugPhysicsColorSettings, island1)       ARC_REFLECT_ATTR(Tooltip, "Island palette, entry 1.")
        ARC_REFLECT_FIELD(DebugPhysicsColorSettings, island2)       ARC_REFLECT_ATTR(Tooltip, "Island palette, entry 2.")
        ARC_REFLECT_FIELD(DebugPhysicsColorSettings, island3)       ARC_REFLECT_ATTR(Tooltip, "Island palette, entry 3.")
        ARC_REFLECT_FIELD(DebugPhysicsColorSettings, island4)       ARC_REFLECT_ATTR(Tooltip, "Island palette, entry 4.")
        ARC_REFLECT_FIELD(DebugPhysicsColorSettings, island5)       ARC_REFLECT_ATTR(Tooltip, "Island palette, entry 5.")
        ARC_REFLECT_FIELD(DebugPhysicsColorSettings, island6)       ARC_REFLECT_ATTR(Tooltip, "Island palette, entry 6.")
        ARC_REFLECT_FIELD(DebugPhysicsColorSettings, island7)       ARC_REFLECT_ATTR(Tooltip, "Island palette, entry 7.")
        ARC_REFLECT_FIELD(DebugPhysicsColorSettings, narrowphase0)  ARC_REFLECT_ATTR(Tooltip, "Manifold colour for a separated (or unknown) narrowphase kind.")
        ARC_REFLECT_FIELD(DebugPhysicsColorSettings, narrowphase1)  ARC_REFLECT_ATTR(Tooltip, "Manifold colour for circle-circle contacts.")
        ARC_REFLECT_FIELD(DebugPhysicsColorSettings, narrowphase2)  ARC_REFLECT_ATTR(Tooltip, "Manifold colour for circle-polygon contacts.")
        ARC_REFLECT_FIELD(DebugPhysicsColorSettings, narrowphase3)  ARC_REFLECT_ATTR(Tooltip, "Manifold colour for capsule contacts.")
        ARC_REFLECT_FIELD(DebugPhysicsColorSettings, narrowphase4)  ARC_REFLECT_ATTR(Tooltip, "Manifold colour for polygon-polygon (SAT) contacts.")
        ARC_REFLECT_FIELD(DebugPhysicsColorSettings, narrowphase5)  ARC_REFLECT_ATTR(Tooltip, "Manifold colour for deep overlaps resolved by EPA.")
        ARC_REFLECT_FIELD(DebugPhysicsColorSettings, narrowphase6)  ARC_REFLECT_ATTR(Tooltip, "Manifold colour for deep overlaps resolved by MPR.")
    ARC_END_REFLECT_TYPE()

    ARC_REFLECT_TYPE(DebugPhysicsTraceSettings)
        ARC_REFLECT_TYPE_ATTR(Settings, "debug.physics.trace", SettingScope::PreferencesProject, ApplyMode::Live, Audience::Game)
        ARC_REFLECT_TYPE_ATTR(Flags, CVarFlags::Dev)
        ARC_REFLECT_FIELD_NAMED(DebugPhysicsTraceSettings, traceLineThickness, "lineThickness")
            ARC_REFLECT_ATTR(Range, 0.5, 8.0)
            ARC_REFLECT_ATTR(Tooltip, "Line thickness (px) of the narrowphase inspector's world overlay.")
        ARC_REFLECT_FIELD(DebugPhysicsTraceSettings, emphasis)
            ARC_REFLECT_ATTR(Range, 0.0, 1.0)
            ARC_REFLECT_ATTR(Tooltip, "Opacity of the inspector overlay when no contact is singled out (1 = full).")
        ARC_REFLECT_FIELD(DebugPhysicsTraceSettings, normalLength)
            ARC_REFLECT_ATTR(Range, 0.01, 100.0)
            ARC_REFLECT_ATTR(Tooltip, "Length (world units) of the inspector's contact normal arrow.")
    ARC_END_REFLECT_TYPE()
}
