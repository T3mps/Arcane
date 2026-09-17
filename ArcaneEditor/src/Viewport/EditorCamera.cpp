#include "Viewport/EditorCamera.hpp"

#include <Arcane/Render/SpriteGeometry.hpp>
#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneResources.hpp>

#include <Astra/Registry/Registry.hpp>

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace Arcane::Editor
{
    namespace
    {
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

        // Task 3 (F1): mat4 world matrix -- the translation moved from
        // column 2 to column 3. Framing stays PLANAR (the editor camera is
        // 2D; F4 owns the 3D one), so the Z of a world position is ignored
        // rather than projected.
        glm::vec2 WorldPositionOf(const glm::mat4& m) noexcept
        {
            return glm::vec2(m[3].x, m[3].y);
        }

        // The drawn quad's XY bounding box: the SAME four world corners
        // RenderSubmissionSystem submits (SpriteWorldQuad -- the full basis
        // about the pivot, F4 plan 1 T5), min/max'd in the plane. One corner
        // rule for drawing and framing, so the two cannot disagree; a rotated
        // sprite frames as the exact AABB of its turned quad, and a negative
        // scale/size simply lands its corners on the other side (min <= max
        // holds by construction).
        void GrowSprite(FramingBounds& b, const glm::mat4& world, const SpriteEntry* entry) noexcept
        {
            const SpriteQuad q = SpriteWorldQuad(world,
                                                 entry ? entry->sizeMeters : glm::vec2(1.0f),
                                                 entry ? entry->pivot      : glm::vec2(0.5f));
            glm::vec2 mn(q.corners[0]), mx(q.corners[0]);
            for (const glm::vec3& c : q.corners)
            {
                mn = glm::min(mn, glm::vec2(c));
                mx = glm::max(mx, glm::vec2(c));
            }
            Grow(b, mn, mx);
        }

        // The sprite asset a SpriteRenderer resolves to, on submission's rules:
        // only a Rect consults the table, and an unresolved sprite is a 1x1 m
        // quad at the centre pivot (RenderSystems.hpp).
        const SpriteEntry* ResolveEntry(const SpriteTable* table, const SpriteRenderer& sprite) noexcept
        {
            return (sprite.shape == SpriteShape::Rect && table) ? table->Resolve(sprite.sprite)
                                                                : nullptr;
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
        const SpriteTable* table = reg.GetResource<SpriteTable>();
        for (Astra::Entity e : entities)
        {
            // No WorldTransform => a dead handle or a non-spatial node: there is
            // no position to frame, so it contributes nothing (not even a count).
            const WorldTransform* world = std::as_const(reg).GetComponent<WorldTransform>(e);
            if (!world)
                continue;

            const SpriteRenderer* sprite = std::as_const(reg).GetComponent<SpriteRenderer>(e);
            if (!sprite)
            {
                // A non-drawn node frames as its bare position.
                const glm::vec2 pos = WorldPositionOf(world->matrix);
                Grow(b, pos, pos);
                continue;
            }
            GrowSprite(b, world->matrix, ResolveEntry(table, *sprite));
        }
        return b;
    }

    FramingBounds SceneFramingBounds(Astra::Registry& reg)
    {
        FramingBounds b;
        const SpriteTable* table = reg.GetResource<SpriteTable>();
        // The SAME view RenderSubmissionSystem submits from, so "frame
        // everything" frames exactly what is on screen.
        reg.CreateView<const WorldTransform, const SpriteRenderer, Astra::Not<Hidden>>().ForEach(
            [&](Astra::Entity, const WorldTransform& world, const SpriteRenderer& sprite)
            {
                GrowSprite(b, world.matrix, ResolveEntry(table, sprite));
            });
        return b;
    }
}
