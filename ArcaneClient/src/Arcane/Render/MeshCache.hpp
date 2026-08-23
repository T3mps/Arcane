#pragma once

// MeshCache: resolves .arcmesh Guids referenced by MeshRenderer::mesh into
// Arcane::MeshEntry records for the scene's MeshTable. Mirrors SpriteCache's
// host integration (SpriteCache.hpp) and its structure almost exactly -- like
// that cache, there is no async compile step here, so Request() resolves
// synchronously in one call: load the .arcmesh JSON (LoadMeshAsset) and
// generate its geometry (BuildMeshData, which validates internally and
// returns nullopt exactly when ValidateMeshAsset refuses the asset), then
// compute its local bounds (ComputeMeshBounds) once alongside it.
//
// FAILURE DISCIPLINE, and this is the one place this cache DIFFERS from
// SpriteCache rather than mirroring it: a failed resolve (unresolvable Guid,
// missing file, malformed JSON, an invalid mesh) stays OUT of the published
// table, in a separate `failed` set -- SpriteMaterialCache's scheme
// (SpriteMaterialCache.cpp:49,97-101), not SpriteCache's own "cache a visible
// placeholder" one. A sprite has a natural placeholder (a 1x1 m untextured
// quad); a mesh does not -- there is no meaningful "wrong shape" to draw
// instead of the right one, and drawing one would be worse than drawing
// nothing. So a nullptr Resolve() is the correct outcome, and
// MeshSubmissionSystem (Task 5) is expected to skip the entity entirely, the
// same way it would for a nil MeshRenderer::mesh.
//
// WHAT IT DOES NOT KEEP: the loaded MeshAssetData. An `AssetFor(Guid) ->
// const MeshAssetData*` accessor lived here through Tasks 4-11, backed by a
// second map beside `table`, on the stated premise that Task 5's material
// chain would read the mesh's own default `material` Guid through it. Task 5
// resolved that differently and better -- MeshEntry::material
// (SceneResources.hpp) carries the Guid on the published table, so the
// submission sweep needs no cache pointer at all -- and no other consumer
// ever appeared: MeshDocument, its one named candidate, edits the SOURCE
// .arcmesh and never touches the resolved cache. Deleted at F2a close with
// zero production callers, which also removes the second map and the
// write-ordering hazard between the two.
//
// ENGINE-SIDE, not editor-side, from the moment it is written -- the same
// placement rule the sprite-resolution lift (2026-07-29) established for
// SpriteCache: this lives in Arcane.dll so both the editor and the
// standalone runtime host draw through the one implementation.

#include <Arcane/Base/Api.hpp>
#include <Arcane/Guid.hpp>
#include <Arcane/Scene/SceneResources.hpp>   // Arcane::MeshEntry (full type: Table()'s value type)

#include <filesystem>
#include <functional>
#include <optional>
#include <unordered_map>

namespace Arcane
{
    class ARCANE_API MeshCache
    {
    public:
        // Guid-shaped, matching SpriteCache/SpriteMaterialCache/PostChainCache
        // exactly, so one host service can hand every cache the same resolver
        // (the AssetId round-trip happens inside).
        using ResolveAssetFn =
            std::function<std::optional<std::filesystem::path>(const Guid&)>;

        struct Services
        {
            ResolveAssetFn resolveAsset;   // Guid -> path (project registry)
        };

        explicit MeshCache(Services services);
        ~MeshCache();
        MeshCache(const MeshCache&) = delete;
        MeshCache& operator=(const MeshCache&) = delete;

        // Ensure `id` is resolved (or known-failed) -- idempotent, a no-op
        // once known (same per-frame sweep contract as SpriteCache::Request).
        // Call per frame per referenced mesh Guid.
        void Request(const Guid& id);

        // Asset re-saved / removed: drop the table (and known-failed) entry
        // so the next Request re-resolves from disk.
        void Invalidate(const Guid& id);

        // Forget everything (project switch): a Guid resolves through the
        // CURRENT project's registry, so a cached entry from the outgoing
        // project may resolve to something else entirely (or nothing) once
        // the project changes.
        void Clear();

        // Guid -> resolved record (the MeshTable payload). Stable storage
        // between mutating calls -- see MeshEntry's own comment
        // (SceneResources.hpp) for exactly what "stable" guarantees here and
        // why (element addresses survive a rehash; only an erase invalidates
        // them).
        const std::unordered_map<Guid, MeshEntry>& Table() const;

    private:
        struct Impl;
        Impl* m_impl = nullptr;
    };
}
