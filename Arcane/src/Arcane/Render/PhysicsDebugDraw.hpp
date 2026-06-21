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
        // in PhysicsDebug.lua.
        bool drawContacts = true;

        // Outline each body's world-space AABB (SlotAabb).  Off by default;
        // useful when debugging the broadphase.
        bool drawAabbs = false;
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
    // currently-begun contact pair (ForEachContact).
    // If opts.drawAabbs, a white outline is drawn for each body's tight AABB
    // (SlotAabb).
    ARCANE_API void DrawPhysicsDebug(
        const Arcane::Physics::PhysicsWorld& world,
        Batcher2D& batcher,
        const PhysicsDebugDrawOptions& opts = {});

} // namespace Arcane
