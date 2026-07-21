#pragma once

// Render module: PickBuffer -- GPU hit-proxy entity picking. A sibling of
// OffscreenCanvas: it owns an R32_UINT id render target (viewport-sized) + a
// 1x1 R32_UINT staging texture + (from Task 3) the entity_id pipeline. Pick()
// renders every pickable entity's silhouette into the id target -- each
// "colored" with a 1-based id, front-most winning -- then reads back the pixel
// under the cursor and maps the id to its Astra::Entity. On-demand: the id pass
// runs ONLY inside Pick(), so there is zero per-frame cost. See
// docs/superpowers/specs/2026-07-19-arcane-entity-id-picking-design.md.
//
// Dimension-agnostic (the reason hit-proxy was chosen over a collider test):
// the pass, id encoding, and readback are identical in 3D -- only the geometry
// each entity emits (CollectPickables, PickEmit.hpp) changes.
//
// Game-agnostic (ARCANE_API): Grimoire owns one beside its OffscreenCanvas and
// calls Pick() on a viewport click; the engine holds no editor state.

#include <Arcane/Base/Api.hpp>
#include <Arcane/Render/PickEmit.hpp>   // PickView, PickDrawable

#include <Astra/Entity/Entity.hpp>

#include <nvrhi/nvrhi.h>

#include <glm/vec2.hpp>

#include <cstdint>
#include <memory>

namespace Astra { class Registry; }

namespace Arcane
{
    class ShaderLibrary;

    class ARCANE_API PickBuffer
    {
    public:
        // Returns null (with ARC_ERROR) if any owned resource fails to build.
        // `shaders` must outlive the returned PickBuffer (the id pipeline keeps a
        // reference for lazy rebuilds, mirroring OffscreenCanvas / Batcher2D).
        static std::unique_ptr<PickBuffer> Create(nvrhi::IDevice* device,
                                                  ShaderLibrary& shaders,
                                                  uint32_t width, uint32_t height);

        virtual ~PickBuffer() = default;

        // THE click-pick primary API: render the entity-id pass and return the
        // entity whose silhouette covers `pixel` (viewport-local, y-down, matching
        // the canvas). Returns an INVALID Astra::Entity when the pixel is
        // background (id 0) or out of range -- an editor clears its selection on
        // an empty-space click. `view` is the same world->canvas transform the
        // scene render uses. One synchronous GPU stall per call (on-demand; not a
        // per-frame path).
        virtual Astra::Entity Pick(Astra::Registry& registry, const PickView& view,
                                   glm::vec2 pixel) = 0;

        // Render the entity-id pass for the current scene into the id target:
        // clears it to 0, then draws every pickable silhouette (CollectPickables
        // order -- back-to-front, front-most LAST) tagged with its 1-based id, so
        // the front-most entity wins a contested pixel. Also rebuilds the internal
        // id<->entity table Pick() looks up. `view` is the same world->canvas
        // transform the scene render uses. This is BOTH the render half of Pick()
        // and the per-frame hover-highlight seam (spec S7).
        virtual void RenderIdPass(Astra::Registry& registry, const PickView& view) = 0;

        // The R32_UINT id render target (valid after Create; rebuilt on Resize).
        // Exposed for readback / a future id-buffer visualization; Pick() reads a
        // single pixel from it internally. (Pick() is added in Task 4.)
        virtual nvrhi::ITexture* IdTarget() const = 0;

        // The pass id assigned to `e` in the most recent RenderIdPass/Pick (k+1
        // order; 0 = background/absent). Lets a consumer (e.g. the selection
        // outline) know an entity's id without a GPU readback.
        virtual uint32_t PassIdOf(Astra::Entity e) const = 0;

        // Tears down and rebuilds the id target at the new size. No-op on a zero
        // dimension or an unchanged size. The 1x1 staging texture is size-
        // independent, so it is not rebuilt.
        virtual void Resize(uint32_t width, uint32_t height) = 0;

        virtual uint32_t Width()  const = 0;
        virtual uint32_t Height() const = 0;
    };
}
