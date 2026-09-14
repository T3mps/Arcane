#pragma once

// NriMeshBufferCache -- ONE Guid -> resident vertex/index buffer pair for the whole
// render path, and the retirement of the per-frame upload ring for mesh geometry.
//
// WHY IT EXISTS (research s3.4-1): MeshNode::Record uploaded every visible mesh's
// streams through the frame's TRANSIENT ring EVERY FRAME -- fine for unit
// primitives, ~3 MB per frame per mesh for a 100k-triangle import. R2's ruling is
// one cache owning CPU+GPU lifetime for ALL meshes, primitives included: the ring
// path for mesh geometry retires outright rather than being kept as a small-mesh
// fast path, because two upload paths is two lifetimes to reason about and the
// resident one is correct for both sizes.
//
// ARCHITECTURE MIRRORS NriTextureCache, deliberately and point for point: an
// injected supply (this class owns no file IO and no Assets knowledge -- a RENDER
// object must not grow a Runtime), failures memoized exactly once, a one-shot WARN
// latch, Release(graveyard, fence) as the sanctioned teardown with the destructor
// as a SAFETY NET that says so at WARN, and Invalidate(id, graveyard, fence) as the
// per-guid escape hatch. Read NriTextureCache.hpp's own banner first; every rule
// there applies here unless this comment says otherwise.
//
// WHAT IS DIFFERENT FROM NriTextureCache, and why:
//   * A BYTE BUDGET AND AN LRU (MeshResidencyBudget.hpp). NriTextureCache has
//     neither -- it never evicts a single entry (NriGraphContext's own note says
//     so). Mesh geometry is where eviction became a real question (F2a's spec
//     assigned it here by name).
//   * NO COLOUR SPACE. A vertex buffer has no sRGB question, so the key is a bare
//     Guid rather than (Guid, space).
//   * NO PLACEHOLDER. A pending texture gets a checkerboard, which is a real,
//     visible, honest stand-in. There is no honest stand-in for a shape (s7.1:
//     "a placeholder cube would fabricate a shape"), so a pending mesh resolves to
//     NOTHING and the draw is skipped -- quietly while pending, loudly when
//     missing or refused.
//
// NO BARRIERS, and none needed -- uploads go through nri::HelperInterface::
// UploadData, which submits and waits internally, so these buffers are NOT graph
// resources. RESOLVE AT DECLARATION TIME ONLY, never inside a node's exec fn: the
// same rule, and the same reason, NriTextureCache states in full.

// Include order: NRI headers first, ALWAYS -- see NriCommon.hpp.
#include <NRI.h>
#include <Extensions/NRIHelper.h>

#include <Arcane/Base/Api.hpp>
#include <Arcane/Guid.hpp>
#include <Arcane/Mesh/MeshAsset.hpp>
#include <Arcane/Render/MeshBuilder.hpp>
#include <Arcane/Render/Nri/MeshResidencyBudget.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>

namespace Arcane
{
    class Graveyard;
    class NriDevice;

    class ARCANE_API NriMeshBufferCache
    {
    public:
        // Guid -> resolved CPU geometry, or a state saying why not. In production this
        // is the frame driver's forward to SceneRenderResolver's MeshTable (already
        // populated by the per-frame Request sweep); the returned pointer is READ
        // INSIDE the call and never stored.
        struct SupplyResult
        {
            const MeshData*    mesh  = nullptr;   // null unless state == Ready
            MeshResolveState   state = MeshResolveState::Failed;
        };
        using MeshSupplyFn = std::function<SupplyResult(const Guid&)>;

        struct Resident
        {
            nri::Buffer*  vertexBuffer = nullptr;
            nri::Buffer*  indexBuffer  = nullptr;
            std::uint32_t indexCount   = 0;
            std::uint64_t bytes        = 0;
            std::uint64_t lastDrawnFrame = 0;
            // The CPU copy is KEPT (re-upload after eviction/device recreate; editor
            // reads) and counted in `bytes` -- see MeshResidencyBudget.hpp.
            MeshData      cpu;
            bool          ready = false;   // upload succeeded; GPU objects are bindable
        };

        static std::unique_ptr<NriMeshBufferCache> Create(NriDevice& device);
        ~NriMeshBufferCache();
        NriMeshBufferCache(const NriMeshBufferCache&)            = delete;
        NriMeshBufferCache& operator=(const NriMeshBufferCache&) = delete;

        void SetMeshSupply(MeshSupplyFn supply) { m_supply = std::move(supply); }

        // The resident pair for `id`, uploading on first sight, and stamping
        // `lastDrawnFrame = frameCounter` on every HIT -- which is what makes the LRU
        // mean "least recently DRAWN" rather than "least recently asked about".
        // Null while the mesh is PendingCook (quiet -- retried on the next call, no
        // memo, because a cook queue is expected to promote it) and null-and-memoized
        // when the supply Failed or the upload was refused.
        [[nodiscard]] const Resident* Resolve(const Guid& id, std::uint64_t frameCounter);

        // Frame-boundary eviction (s7.2). Called ONCE per frame by the vehicle, AFTER
        // recording and BEFORE the next frame's declarations, so nothing evicted here
        // can be named by a command buffer still being built. Buries evicted buffers at
        // `fence`, so an ALREADY-RECORDED frame still reading them is never invalidated
        // out from under it -- the same discipline NriTextureCache::Invalidate keeps.
        // Reports ONCE, at WARN, when the protected set alone exceeds the budget.
        //
        // `budget` defaults to kMeshResidencyBudgetBytes. Tests pass a smaller value so
        // a handful of cubes can exercise the LRU without filling 512 MiB.
        void EvictToBudget(std::uint64_t frameCounter, Graveyard& graveyard,
                           std::uint64_t fence,
                           std::uint64_t budget = kMeshResidencyBudgetBytes);

        // Drops residency for `id` so the NEXT Resolve treats it as brand new --
        // Resident, Pending or Refused, whichever it lands in. The escape hatch a
        // cook-completion callback needs (s7.3), and the un-latch a Refused key would
        // otherwise never get.
        void Invalidate(const Guid& id, Graveyard& graveyard, std::uint64_t fence);

        // Buries every NRI object this cache owns at `fence` and empties it.
        // Idempotent. THE DEVICE-LOSS / RECREATE HOOK TOO: a recreated device's
        // buffers are new objects, and because the CPU copy is kept, the next Resolve
        // re-uploads from memory rather than re-reading an artifact from disk.
        void Release(Graveyard& graveyard, std::uint64_t fence);

        [[nodiscard]] std::size_t   ResidentCount() const noexcept;
        [[nodiscard]] std::uint64_t ResidentBytes() const noexcept;

    private:
        NriMeshBufferCache() = default;

        void Bury(Resident& r, Graveyard& graveyard, std::uint64_t fence);
        bool Upload(Resident& r, const Guid& id);

        [[nodiscard]] static std::size_t SectionBytes(const MeshData& mesh) noexcept;

        NriDevice*           m_device = nullptr;
        nri::HelperInterface m_helper{};
        MeshSupplyFn         m_supply;
        std::unordered_map<Guid, Resident> m_entries;
        bool m_warnedOverBudget = false;
        bool m_warnedMiss       = false;
    };
}
