#pragma once

// MeshAsset: the .arcmesh file -- a native JSON asset with an embedded
// top-level "id" (rides AssetRegistry::ScanContent's native path, exactly like
// .arcsprite and .arcmat). It names a procedural GENERATOR plus the topology
// that generator needs, and a default material.
//
// THE UNIT RULE, which decides every field here: generators emit UNIT
// geometry. Scale expresses size, rotation expresses orientation, the asset
// expresses shape. There is no sizeMeters and no radiusMeters -- the engine
// already ruled this on SpriteRenderer ("There is NO size field: an entity is
// sized by its Transform scale"), and BOTH reference engines confirm it for
// meshes: neither UStaticMesh nor a Source 2 model stores a size, and UE reads
// streaming scale straight off the component transform
// (StaticMeshComponent.h:684).
//
// FLAT AND TAGGED, not a variant: per-source field meaning is documented
// rather than enforced by the type, which is the same form
// Manifold2D::Physics::Shape uses (halfLen simply means nothing to a Circle).
// Its one real hazard is validating the whole struct regardless of tag, which
// would refuse legal assets -- see ValidateMeshAsset.
//
// F2c's SEAM: an imported mesh becomes another MeshSource plus an artifact
// reference, with no component and no scene change.

#include <Arcane/Base/Api.hpp>
#include <Arcane/Guid.hpp>
#include <Arcane/Render/MeshBuilder.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace Arcane
{
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4251)  // std members on dll-exported types: benign under /MD
#endif

    // The generator roster -- Unity's built-in set, minus Quad (which is Plane
    // under a +90 degree X rotation, right-handed: that takes BuildPlane's +Y
    // normal to +Z, facing a camera that looks down -Z. Orientation is the
    // Transform's job for the same reason size is; see BuildPlane's own
    // comment in Render/MeshBuilder.hpp).
    //
    // Explicitly uint8_t-backed and explicitly numbered: these values are
    // PERSISTED, so reordering them silently re-authors every .arcmesh in
    // every project.
    enum class MeshSource : std::uint8_t
    {
        Plane    = 0,
        Cube     = 1,
        UvSphere = 2,
        Cylinder = 3,
        Capsule  = 4,
    };

    struct MeshAssetData
    {
        Guid          id{};
        std::string   name;
        MeshSource    source = MeshSource::Cube;

        // ---- TOPOLOGY: density a Transform cannot express ----------------
        std::uint32_t rings        = 16;   // UvSphere; Capsule (arc steps per cap)
        std::uint32_t segments     = 32;   // UvSphere, Cylinder, Capsule (radial)
        std::uint32_t subdivisions = 1;    // Plane (quads per axis; 1 = one quad)

        // ---- SHAPE RATIO: a family no scale can reach --------------------
        // Total height / diameter. A cylinder needs no equivalent because a
        // cylinder scaled in Y is still a cylinder; a capsule scaled in Y is
        // not (its hemispherical caps become ellipsoids). That is the test any
        // future shape parameter must pass.
        float capsuleLengthRatio = 2.0f;

        // ---- The mesh's DEFAULT material, overridable per entity ---------
        // Both reference engines put assignment on the asset:
        // UStaticMesh::StaticMaterials (StaticMesh.h:1095) and Source 2's
        // m_materialGroups. MeshRenderer::materialOverride is
        // UMeshComponent::OverrideMaterials in miniature. Nil = white.
        //
        // SCALAR, not an array, because F2a's primitives are single-section.
        // F2c's imported multi-section meshes grow this into a slot array,
        // which is ADDITIVE -- putting it on the component instead would have
        // forced a later MOVE (a component schema change plus a scene
        // re-author).
        Guid material{};
    };

    // Memberwise equality, for the same reason SpriteAssetData has one: the
    // document's undo bracket compares an activation-time COPY against the live
    // data to decide whether a drag actually moved anything. Memberwise and
    // never memcmp -- `name` is a std::string (its object bytes are a
    // pointer/SSO buffer, not the text) and the struct is padded.
    [[nodiscard]] inline bool operator==(const MeshAssetData& a, const MeshAssetData& b) noexcept
    {
        return a.id == b.id && a.name == b.name && a.source == b.source &&
               a.rings == b.rings && a.segments == b.segments &&
               a.subdivisions == b.subdivisions &&
               a.capsuleLengthRatio == b.capsuleLengthRatio &&
               a.material == b.material;
    }

    // Write `data` as .arcmesh JSON. EVERY field is written, regardless of
    // which fields the current source reads -- unlike .arcsprite's sparse
    // rect/pivot write, a sparse write here has a real defect: set a
    // capsule's rings, switch `source` to Plane, save, switch back, and
    // rings silently reverts to its default. Writing every field is what
    // lets the editor round-trip a source switch without discarding the
    // other source's topology. False on IO failure.
    ARCANE_API bool SaveMeshAsset(const std::filesystem::path& path, const MeshAssetData& data);

    // Parse a .arcmesh. nullopt on IO/parse failure or when the file is not a
    // mesh asset (the "type":"mesh" tag IS the discriminator, exactly as
    // .arcsprite works -- no structurally-unique key distinguishes one).
    // Malformed INDIVIDUAL fields fall back to their MeshAssetData default
    // rather than failing the whole load.
    ARCANE_API std::optional<MeshAssetData> LoadMeshAsset(const std::filesystem::path& path);

    // nullopt == valid. Otherwise the human-readable reason, NAMING THE FIELD
    // -- it is what a user reads in the Problems pane, and a reason that does
    // not name the field is not actionable.
    //
    // Evaluated PER SOURCE, over the fields that source actually reads: a Plane
    // with segments == 0 is VALID, because a plane has no radial segments.
    // Validating the whole struct regardless of tag would refuse legal assets
    // and is the flat struct's one real hazard.
    //
    // Thresholds are UE's, from GeometryCore's generators
    // (SphereGenerator.h:200-201, CapsuleGenerator.h:265-267). UE CLAMPS
    // silently where this refuses, and UE is not simply right: its generators
    // are tool-time transients where a clamp is invisible, while .arcmesh is
    // PERSISTED -- a silent clamp leaves the file saying 1 while the mesh is 3,
    // forever. MeshDocument's param panel also bounds every one of these at the
    // WIDGET (bounded DragInt/DragFloat, the idiom SpriteDocument already
    // uses), so in practice refusal only ever fires on a hand-edited file.
    [[nodiscard]] ARCANE_API std::optional<std::string> ValidateMeshAsset(const MeshAssetData& data);

    // Generate the geometry. nullopt exactly when ValidateMeshAsset returns a
    // reason -- an INVALID mesh is an error and emits nothing, where a NIL mesh
    // Guid on a component is not an error at all (it draws nothing, like a nil
    // sprite).
    //
    // DETERMINISTIC: same input, same bytes. F2c inherits this builder into a
    // cook step whose artifacts must be reproducible.
    [[nodiscard]] ARCANE_API std::optional<MeshData> BuildMeshData(const MeshAssetData& data);

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}
