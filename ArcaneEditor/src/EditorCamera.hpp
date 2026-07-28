#pragma once

// The EDITOR's own viewport camera: pure state + pure math, no ImGui and no
// engine calls, so the [editor] units drive it headlessly (same split as
// ViewportInput.hpp / SceneSession.hpp; the host performs every effect).
//
// WHY the editor owns a camera at all: Runtime::SetCamera is the PLUGIN's seam
// (Loom: "plugin drives via Runtime::SetCamera, default identity if it never
// does"), and a project whose game module never calls it gets offset (0,0) and
// zoom 1. An authoring tool cannot depend on the game implementing navigation,
// so EditorApp drives this camera from viewport input and pushes it in Edit
// mode; in Play the plugin's camera wins so the game looks like the game.
//
// CANONICAL TRANSFORM (the engine's, byte-for-byte -- RenderContext2D,
// RenderSubmissionSystem, GizmoView and PickView all apply exactly this):
//
//     screen = world * zoom + offset
//     world  = (screen - offset) / zoom
//
// `zoom` is therefore the world->SCREEN scale, and world is METRES (MKS), so
// zoom reads as PIXELS PER METRE. That is why kDefaultZoom is 100 and not 1 --
// it is the same px-per-metre Sandbox folds into the value it pushes
// (Sandbox::Camera::WorldToScreenScale), and at zoom 1 a 1 m body would render
// one pixel tall. `offset` is in viewport-local pixels (top-left origin, +y
// down), the space ViewportInput::ToViewportLocal produces.

#include <glm/vec2.hpp>

#include <cstddef>
#include <span>

#include <Astra/Entity/Entity.hpp>

namespace Astra { class Registry; }

namespace Arcane::Editor
{
    struct EditorCamera
    {
        // 100 px per metre: content authored in MKS is visible on first open.
        static constexpr float kDefaultZoom = 100.0f;

        // Zoom clamp, in px per metre: two decades either side of the default.
        // kMinZoom = 1 px/m is the coarsest scale at which a 1 m body is still
        // one whole pixel -- below it the scene is sub-pixel and the user has
        // lost it; kMaxZoom = 10000 px/m puts 100 px on a 1 cm feature, which
        // is past any placement precision the 2D scene needs. Both ends are
        // finite and positive, so ScreenToWorld can never divide by zero and
        // the scene can never be scrolled to a scale it cannot come back from.
        static constexpr float kMinZoom = 1.0f;
        static constexpr float kMaxZoom = 10000.0f;

        // Multiplicative zoom per wheel tick. Matches the Sandbox camera's
        // kZoomWheelStep so wheel feel is the same in the editor and in a
        // running game.
        static constexpr float kWheelStep = 1.12f;

        // Fraction of the viewport a framed AABB spans on its fitted axis:
        // 10% total padding, 5% a side, so a framed object is not flush against
        // the panel edge.
        static constexpr float kFrameFill = 0.9f;

        glm::vec2 offset{0.0f, 0.0f};       // viewport-local px
        float     zoom = kDefaultZoom;      // px per world-metre

        [[nodiscard]] glm::vec2 WorldToScreen(glm::vec2 world) const noexcept;
        [[nodiscard]] glm::vec2 ScreenToWorld(glm::vec2 screen) const noexcept;

        // RMB drag: the view follows the cursor 1:1, so the raw screen-px delta
        // IS the offset shift.
        void Pan(glm::vec2 screenDelta) noexcept;

        // Wheel zoom ANCHORED at screenPos: the world point under the cursor is
        // still under the cursor afterwards. wheelTicks is the accumulated
        // per-frame wheel delta (InputSnapshot::wheelY; +y = in). A tick count
        // that would leave the zoom unchanged (0, or more of the same at a
        // clamp) is a no-op, so a pinned camera cannot drift.
        void ZoomAt(glm::vec2 screenPos, float wheelTicks) noexcept;

        // Fit [worldMin, worldMax] centred in a viewportSize-pixel panel with
        // kFrameFill margin. A zero-extent axis cannot imply a scale, so it is
        // ignored; a wholly zero-extent AABB (a point) only re-centres. A
        // viewport with a zero (or negative) dimension leaves the camera
        // untouched -- there is nothing to fit into.
        void Frame(glm::vec2 worldMin, glm::vec2 worldMax, glm::vec2 viewportSize) noexcept;
    };

    // World-space AABB for framing. `count` is the number of entities that
    // actually contributed, which is what separates "nothing framable here"
    // (count 0 -- the caller must do nothing) from "a real but zero-extent box"
    // (count > 0 with min == max, e.g. one transform-only node).
    struct FramingBounds
    {
        glm::vec2   min{0.0f, 0.0f};
        glm::vec2   max{0.0f, 0.0f};
        std::size_t count = 0;

        [[nodiscard]] bool Valid() const noexcept { return count > 0; }
    };

    // Bounds over an explicit entity set (Frame Selected). A sprite contributes
    // the box RenderSubmissionSystem draws it in -- its sprite asset's base size
    // (1x1 m when unresolved) times the world scale, about the asset's pivot --
    // so framing and rendering cannot disagree. An entity with a WorldTransform but no
    // SpriteRenderer contributes its position as a zero-extent point, so
    // framing a bare node centres on it instead of doing nothing. Entities with
    // no WorldTransform (and dead handles) are skipped. Rotation is NOT
    // accounted for: the box uses the axis-aligned extent of the unrotated
    // sprite, so a rotated sprite can overhang it by up to its half-diagonal.
    [[nodiscard]] FramingBounds SelectionFramingBounds(Astra::Registry& reg,
                                                       std::span<const Astra::Entity> entities);

    // Bounds over every VISIBLE sprite in the scene (Frame All / Home).
    // Sprite-only and Hidden-excluded, i.e. exactly what the renderer draws:
    // bare transform nodes (the SceneRoot at the origin among them) would
    // otherwise drag the box back toward (0,0), and hidden entities would
    // stretch it out to things the user cannot see.
    [[nodiscard]] FramingBounds SceneFramingBounds(Astra::Registry& reg);
}
