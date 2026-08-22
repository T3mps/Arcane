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

#include <vector>

namespace Arcane
{
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
    // default material (MeshEntry::material -- a copy of the loaded
    // .arcmesh's MeshAssetData::material, made once at MeshCache::Request
    // time, see SceneResources.hpp and MeshCache.cpp), if THAT resolves;
    // else white (1,1,1,1). MeshMaterialTable::Resolve already folds "nil
    // Guid" and "valid Guid, not (yet, or ever) in the table" into the same
    // nullptr outcome (SceneResources.hpp), so the two-step fallback below
    // is the whole chain -- no separate branch for "nil" vs. "broken
    // reference" is needed at this call site.
    //
    // WARN-ONCE, WITHOUT a function-local static or a caller-supplied memo:
    // this function never calls Request() on either cache-backed table (see
    // below), so it never triggers a resolve attempt in the first place.
    // MeshMaterialCache::Request is what warns -- exactly once per broken
    // Guid, memoized into its own private `failed` set (MeshMaterialCache.
    // cpp's `fail` lambda) -- and that happens the FIRST time some earlier
    // sweep (the resolver's per-frame Request loop, a later task) resolves
    // it. By the time THIS function runs, a materialOverride Guid is
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
    // MeshMaterialTable is the resolver's per-frame job (a later task), not
    // this sweep's -- this function reads ONLY the already-published
    // resources.
    //
    // NO MeshData COPY: MeshInstance::mesh borrows a raw pointer straight
    // into the MeshTable entry's owned MeshData, which the resolver's
    // MeshCache owns and keeps alive well past this call and past the
    // RenderFrame call that consumes `out` -- see MeshInstance's own
    // borrowing-contract comment (MeshNode.hpp:103-107) and MeshEntry's
    // (SceneResources.hpp).
    inline void CollectMeshInstances(Astra::Registry& reg, std::vector<MeshInstance>& out)
    {
        out.clear();

        const MeshTable*         meshTable = reg.GetResource<MeshTable>();
        const MeshMaterialTable* matTable  = reg.GetResource<MeshMaterialTable>();

        auto view = reg.CreateView<WorldTransform, MeshRenderer, Astra::Not<Hidden>>();
        view.ForEach([&](Astra::Entity, WorldTransform& world, MeshRenderer& renderer)
        {
            // Nil or unresolved mesh: nothing to draw, and no placeholder --
            // see MeshTable's own comment (SceneResources.hpp) for why a
            // broken mesh reference draws nothing rather than the wrong
            // shape.
            const MeshEntry* entry = meshTable ? meshTable->Resolve(renderer.mesh) : nullptr;
            if (!entry)
                return;

            const ResolvedMeshMaterial* mat =
                matTable ? matTable->Resolve(renderer.materialOverride) : nullptr;
            if (!mat)
                mat = matTable ? matTable->Resolve(entry->material) : nullptr;
            const glm::vec4 baseColor = mat ? mat->baseColor : glm::vec4(1.0f);

            out.push_back(MeshInstance{ &entry->data, world.matrix, baseColor });
        });
    }
}
