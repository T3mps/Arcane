#include "EditorCamera.hpp"

#include <Arcane/Scene/Components.hpp>

#include <Astra/Registry/Registry.hpp>

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace Arcane::Editor
{
    namespace
    {
        // World scale from a world matrix, as basis-column lengths -- the same
        // derivation RenderSubmissionSystem uses to size the drawn quad.
        glm::vec2 WorldScaleOf(const glm::mat3& m) noexcept
        {
            return glm::vec2(glm::length(glm::vec2(m[0])), glm::length(glm::vec2(m[1])));
        }

        void Grow(FramingBounds& b, glm::vec2 mn, glm::vec2 mx) noexcept
        {
            if (b.count == 0)
            {
                b.min = mn;
                b.max = mx;
            }
            else
            {
                b.min = glm::min(b.min, mn);
                b.max = glm::max(b.max, mx);
            }
            ++b.count;
        }

        // Sprite half-extent in world units: SpriteRenderer.size times the world
        // scale, halved -- the sprite is drawn CENTRED on its world position
        // (RenderSystems.hpp). abs() keeps min <= max, which Frame relies on; a
        // negatively authored size would otherwise invert the box.
        glm::vec2 SpriteHalfExtent(const glm::mat3& world, const SpriteRenderer& sprite) noexcept
        {
            return glm::abs(sprite.size * WorldScaleOf(world)) * 0.5f;
        }

        glm::vec2 WorldPositionOf(const glm::mat3& m) noexcept
        {
            return glm::vec2(m[2].x, m[2].y);
        }
    }

    glm::vec2 EditorCamera::WorldToScreen(glm::vec2 world) const noexcept
    {
        return world * zoom + offset;
    }

    glm::vec2 EditorCamera::ScreenToWorld(glm::vec2 screen) const noexcept
    {
        return (screen - offset) / zoom;
    }

    void EditorCamera::Pan(glm::vec2 screenDelta) noexcept
    {
        offset += screenDelta;
    }

    void EditorCamera::ZoomAt(glm::vec2 screenPos, float wheelTicks) noexcept
    {
        // Multiplicative so every notch is the same perceived step, and so a
        // multi-notch or fractional per-frame delta lands where the same number
        // of single notches would.
        const float next = std::clamp(zoom * std::pow(kWheelStep, wheelTicks), kMinZoom, kMaxZoom);
        // A non-finite wheelTicks propagates through pow/clamp as NaN, which
        // fails `> 0.0f` -- so garbage input leaves the camera alone rather
        // than poisoning offset. `next == zoom` covers both a zero-tick wheel
        // and more of the same notch at a clamp; without it the re-solve below
        // would nudge offset by a rounding error every such frame.
        if (!(next > 0.0f) || next == zoom)
            return;

        // Anchor: measure the world point under the cursor at the OLD zoom, then
        // re-solve offset from screen = world * zoom + offset at the new one, so
        // that point maps back to the same pixel.
        const glm::vec2 anchored = ScreenToWorld(screenPos);
        zoom   = next;
        offset = screenPos - anchored * zoom;
    }

    void EditorCamera::Frame(glm::vec2 worldMin, glm::vec2 worldMax, glm::vec2 viewportSize) noexcept
    {
        // A zero (or negative, or non-finite) viewport has nothing to fit into,
        // and would divide the centring below by zero.
        if (!(viewportSize.x > 0.0f) || !(viewportSize.y > 0.0f))
            return;

        // Tolerate a caller that hands the corners in either order.
        const glm::vec2 lo = glm::min(worldMin, worldMax);
        const glm::vec2 hi = glm::max(worldMin, worldMax);
        const glm::vec2 extent = hi - lo;

        // Fit the TIGHTER axis so the whole box lands on screen. An axis with no
        // extent cannot imply a scale and is skipped; if neither has extent (a
        // single point) the zoom is left alone and this only re-centres.
        float fit = 0.0f;
        if (extent.x > 0.0f)
            fit = viewportSize.x * kFrameFill / extent.x;
        if (extent.y > 0.0f)
        {
            const float fitY = viewportSize.y * kFrameFill / extent.y;
            fit = (fit > 0.0f) ? std::min(fit, fitY) : fitY;
        }
        if (fit > 0.0f)
            zoom = std::clamp(fit, kMinZoom, kMaxZoom);

        // Put the AABB centre on the viewport centre. Note the clamp above can
        // stop a very large or very small box from fitting exactly; it stays
        // centred either way.
        const glm::vec2 centre = (lo + hi) * 0.5f;
        offset = viewportSize * 0.5f - centre * zoom;
    }

    FramingBounds SelectionFramingBounds(Astra::Registry& reg,
                                         std::span<const Astra::Entity> entities)
    {
        FramingBounds b;
        for (Astra::Entity e : entities)
        {
            // No WorldTransform => a dead handle or a non-spatial node: there is
            // no position to frame, so it contributes nothing (not even a count).
            const WorldTransform* world = reg.GetComponent<WorldTransform>(e);
            if (!world)
                continue;

            const glm::vec2 pos = WorldPositionOf(world->matrix);
            const SpriteRenderer* sprite = reg.GetComponent<SpriteRenderer>(e);
            const glm::vec2 half = sprite ? SpriteHalfExtent(world->matrix, *sprite)
                                          : glm::vec2(0.0f);
            Grow(b, pos - half, pos + half);
        }
        return b;
    }

    FramingBounds SceneFramingBounds(Astra::Registry& reg)
    {
        FramingBounds b;
        // The SAME view RenderSubmissionSystem submits from, so "frame
        // everything" frames exactly what is on screen.
        reg.CreateView<WorldTransform, SpriteRenderer, Astra::Not<Hidden>>().ForEach(
            [&](Astra::Entity, WorldTransform& world, SpriteRenderer& sprite)
            {
                const glm::vec2 pos  = WorldPositionOf(world.matrix);
                const glm::vec2 half = SpriteHalfExtent(world.matrix, sprite);
                Grow(b, pos - half, pos + half);
            });
        return b;
    }
}
