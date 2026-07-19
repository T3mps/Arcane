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

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <memory>

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

        // Tears down and rebuilds the id target at the new size. No-op on a zero
        // dimension or an unchanged size. The 1x1 staging texture is size-
        // independent, so it is not rebuilt. (Pick() is added in Task 4.)
        virtual void Resize(uint32_t width, uint32_t height) = 0;

        virtual uint32_t Width()  const = 0;
        virtual uint32_t Height() const = 0;
    };
}
