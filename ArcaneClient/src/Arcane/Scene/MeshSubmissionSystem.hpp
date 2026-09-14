#pragma once

// MeshSubmissionSystem (F2a, Task 5): turns a scene's MeshRenderer entities
// into the std::vector<MeshInstance> a host hands to MeshSceneDesc for
// RenderFrame (Render/Nri/nodes/MeshNode.hpp). Mirrors RenderSubmissionSystem's
// sweep idiom (Scene/RenderSystems.hpp:47) almost exactly.
//
// A FREE FUNCTION, not an Astra::System, because the output vector is
// HOST-OWNED: it must outlive the RenderFrame call that borrows into it (see
// the NO MeshData COPY note below), and a system has nowhere to put that
// ownership. RenderSubmissionSystem gets to be a system because its sink --
// the Batcher2D -- lives in a resource (RenderContext2D::batcher); this sweep
// has no equivalent sink, so the CALLER supplies (and keeps alive) the
// vector instead.
//
// HEADER-ONLY, DELIBERATELY: EditorAppFrame.cpp and RuntimeFrame.cpp (the
// two host frame drivers) are NOT compiled into ArcaneTests, so any sweep
// logic living in one of those .cpp files would have zero test coverage.
// Keeping this in a header, callable directly from ArcaneTests, is what
// makes the whole chain (four material-resolution states, four skip
// conditions) testable without a device, a compiler or a Runtime.
//
// Include order: NRI headers first, ALWAYS -- MeshInstance is declared in
// MeshNode.hpp, which pulls <NRI.h> and (via NriPipelineCache.hpp ->
// NriDevice.hpp) <Extensions/NRIDeviceCreation.h>, which declares
// nri::Message::ERROR; <windows.h> (reachable through Arcane/Base/Log.hpp ->
// spdlog, which other engine headers can pull in) #defines ERROR via
// wingdi.h. Keeping MeshNode.hpp the FIRST include in this file (see
// MeshNode.hpp's own header comment and NriGraphContext.hpp:222, the same
// convention) is what keeps that macro clash from ever mattering here.
#include <Arcane/Render/Nri/nodes/MeshNode.hpp>   // MeshInstance

#include <Arcane/Scene/Components.hpp>
#include <Arcane/Scene/SceneResources.hpp>

#include <Astra/Registry/Registry.hpp>

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace Arcane
{
    // F2b Task 11: SceneResources.hpp's ResolvedMeshMaterial::materialSlot
    // restates BindlessTable::kInvalidSlot as a plain 0xFFFFFFFF literal
    // rather than including Render/Nri/BindlessTable.hpp (that header pulls
    // <NRI.h>, and SceneResources.hpp is deliberately render-backend-
    // agnostic -- see that field's own comment for the full reasoning). This
    // is the ONE place both symbols are visible together (this header
    // already pulls MeshNode.hpp -> BindlessTable.hpp for MeshInstance
    // itself), so it is where the two literals' agreement is actually
    // checked, not just asserted in a comment.
    static_assert(std::uint32_t{0xFFFFFFFFu} == BindlessTable::kInvalidSlot,
                  "SceneResources.hpp's ResolvedMeshMaterial::materialSlot default must mirror "
                  "BindlessTable::kInvalidSlot exactly");

    // Sweeps every (WorldTransform, MeshRenderer) entity minus Hidden,
    // resolving each into a borrowed-mesh MeshInstance and appending it to
    // `out`. An entity missing WorldTransform is excluded by the view itself
    // (CreateView requires both components present), matching
    // RenderSubmissionSystem's own precedent for "missing a required
    // component" -- no separate branch needed.
    //
    // `out` is CLEARED ON ENTRY: this is meant to be called once per frame
    // and rebuild the whole list from the current scene, not accumulate
    // across calls -- a second call against a scene that lost an entity
    // must not leave that entity's stale MeshInstance behind.
    //
    // THE MATERIAL CHAIN (see MeshRenderer's own comment, Components.hpp):
    // materialOverride, if it resolves, wins; else the mesh asset's own
    // default material (MeshEntry::slots -- a copy of the loaded .arcmesh's
    // MeshAssetData::slots, made once at MeshCache::Request time, see
    // SceneResources.hpp and MeshCache.cpp), if THAT resolves; else white
    // (1,1,1,1). MeshMaterialTable::Resolve already folds "nil Guid" and
    // "valid Guid, not (yet, or ever) in the table" into the same nullptr
    // outcome (SceneResources.hpp), so the two-step fallback below is the
    // whole chain -- no separate branch for "nil" vs. "broken reference" is
    // needed at this call site.
    //
    // F2c Task 10 DEFERRAL (spec s4.4, restated at this header's top): a
    // mesh asset can now carry MULTIPLE slots, one per section, but this
    // sweep still emits ONE MeshInstance per ENTITY and resolves through
    // slots[0] only -- byte-identical behaviour for every F2a mesh (which
    // carries at most one slot). Emitting one instance PER SECTION (so a
    // multi-slot imported mesh's other sections stop being invisible) is
    // Plan 2's Task 5, which is also what makes the DRAW side (MeshNode)
    // consume a per-section submission at all; doing it here first would
    // leave sections 1..N submitted but never drawn.
    //
    // WARN-ONCE, WITHOUT a function-local static or a caller-supplied memo:
    // this function never calls Request() on either cache-backed table (see
    // below), so it never triggers a resolve attempt in the first place.
    // MeshMaterialCache::Request is what warns -- exactly once per broken
    // Guid, memoized into its own private `failed` set (MeshMaterialCache.
    // cpp's `fail` lambda) -- and that happens the FIRST time some earlier
    // sweep resolves it: the resolver's per-frame Request loop, landed at
    // Host/SceneRenderResolver.cpp:357-378 (Task 6). By the time THIS
    // function runs, a materialOverride Guid is
    // already either resolved or already-warned-and-memoized; either way,
    // Resolve() here is a pure lookup with no side effect of its own. This
    // sweep is therefore silent by construction: there is no local warn
    // state to keep (static or otherwise), so nothing here is order-
    // dependent under `--order rand`. The fall-through itself is still
    // fully observable to a test without needing to observe the warn: give
    // the mesh default a distinct baseColor from the override candidate
    // and from white, point materialOverride at a Guid that is valid but
    // absent from the table (exactly what an already-failed Request leaves
    // behind), and check the resulting MeshInstance carries the mesh
    // default's colour, not white and not a colour that was never in
    // either table.
    //
    // NO Request() CALL, ON EITHER TABLE: populating MeshTable/
    // MeshMaterialTable is the resolver's per-frame job (SceneRenderResolver::
    // Refresh, sweep (1b) at Host/SceneRenderResolver.cpp:357-378), not this
    // sweep's -- this function reads ONLY the already-published resources.
    //
    // MeshInstance::mesh is the component's Guid (F2c s7.2). Geometry is made
    // resident by NriMeshBufferCache at declaration time; this sweep no longer
    // borrows MeshEntry::data. The MeshTable lookup still decides whether the
    // entity is drawable at all -- a Guid not in the table is skipped, same as
    // before.
    inline void CollectMeshInstances(Astra::Registry& reg, std::vector<MeshInstance>& out)
    {
        out.clear();

        const MeshTable*         meshTable = reg.GetResource<MeshTable>();
        const MeshMaterialTable* matTable  = reg.GetResource<MeshMaterialTable>();

        auto view = reg.CreateView<const WorldTransform, const MeshRenderer, Astra::Not<Hidden>>();
        view.ForEach([&](Astra::Entity, const WorldTransform& world, const MeshRenderer& renderer)
        {
            // Nil or unresolved mesh: nothing to draw, and no placeholder --
            // see MeshTable's own comment (SceneResources.hpp) for why a
            // broken mesh reference draws nothing rather than the wrong
            // shape.
            const MeshEntry* entry = meshTable ? meshTable->Resolve(renderer.mesh) : nullptr;
            if (!entry)
                return;

            // slots[0] when a slot exists, nil otherwise -- see the DEFERRAL
            // note above this function for why "when a slot exists" is the
            // whole per-section story this task tells.
            const Guid meshDefaultMaterial =
                entry->slots.empty() ? Guid{} : entry->slots[0].material;

            const ResolvedMeshMaterial* mat =
                matTable ? matTable->Resolve(renderer.materialOverride) : nullptr;
            if (!mat)
                mat = matTable ? matTable->Resolve(meshDefaultMaterial) : nullptr;
            const glm::vec4 baseColor = mat ? mat->baseColor : glm::vec4(1.0f);
            // F2b Task 11: the resolved material's bindless slot, already
            // resolved by the time this sweep runs -- SceneRenderResolver::
            // Refresh's per-frame MeshMaterialCache::Request calls
            // (Host/SceneRenderResolver.cpp) are what actually reach the
            // device; this function calls neither cache (see the NO
            // Request() CALL note above) and only copies the value across.
            // No material resolved at all (nil override AND nil mesh
            // default, or a broken override that fell through) means no
            // slot either -- MeshInstance::materialSlot's own default,
            // BindlessTable::kInvalidSlot, the flat baseColor path.
            const std::uint32_t materialSlot = mat ? mat->materialSlot : BindlessTable::kInvalidSlot;

            out.push_back(MeshInstance{ renderer.mesh, world.matrix, baseColor, materialSlot });
        });
    }
}
