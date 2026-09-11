#pragma once

// Arcane::AssetPipeline::MeshImporter -- mesh importer v1, the glTF path. Shape mirrors
// TextureImporter.hpp:42-55, with ONE deliberate difference: a refusal carries its
// own REASON rather than collapsing to nullopt. TextureImporter can afford nullopt
// because there is exactly one way a .png fails (it did not decode); a glTF has
// several -- nothing drawable, an unsupported required extension, a validate
// failure -- and s4.5 requires the diagnostic to NAME which. "Naming every
// unsupported entry in one diagnostic" is not expressible through a bare nullopt.
//
// F2c Task 6 lands the front half only: cgltf_parse, the mandatory cgltf_validate
// pass, and every refusal the importer owes (spec s4.5's refusal ladder, in the
// order applied):
//   1. cgltf_parse fails                    -> refuse, naming the cgltf result
//   2. extensionsRequired carries anything   -> refuse, naming EVERY unsupported
//      this importer does not implement         entry in ONE diagnostic (A2 part 1)
//   3. cgltf_load_buffers fails              -> refuse, naming the missing buffer
//   4. cgltf_validate fails                  -> refuse. MANDATORY -- the CVE the
//                                                vendor pin exists for (s4.5/s10)
//   5. nothing drawable (no meshes, or       -> refuse, naming the file (s4.5;
//      every triangle degenerate FILE-wide)     UE's Error_NoPolygonFoundInMesh tier)
// A degenerate triangle inside an otherwise valid primitive is NOT a refusal: it is
// dropped with one warning naming its primitive (A2 part 2 -- the case an
// implementer actually meets; refusing a 50k-triangle prop over three bad faces is
// precisely the failure that amendment exists to prevent). F2c Task 7 lands the actual
// vertex/index bake (`ImportedMesh`) over these same cases -- `mesh` is now set
// whenever `refusal` is empty (see ImportMesh's own comment below for the bake
// pipeline: flatten, bake, winding flip, sections/slots, remap/optimize, AABB).

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "Arcane/AssetPipeline/ArtifactFormat.hpp"
#include "Arcane/AssetPipeline/MeshMetaSettings.hpp"
#include "Arcane/Guid.hpp"

namespace Arcane::AssetPipeline
{
    struct ImportedMesh
    {
        MeshArtifactDesc                desc;
        std::vector<MeshArtifactVertex> vertices;
        std::vector<std::uint32_t>      indices;
    };

    struct MeshImportResult
    {
        std::optional<ImportedMesh> mesh;        // set iff refusal.empty()
        std::string                 refusal;     // human-readable; non-empty == refused
        std::vector<std::string>    warnings;    // s4.5 tier 2 -- dropped degenerates
    };

    // Parses `sourceBytes` as glTF or GLB (cgltf sniffs the container) and imports it.
    // `externalBuffers` is ReadExternalBuffers' own result for this source, in glTF
    // declaration order -- passed IN rather than re-read so the artifact's sourceHash
    // and ComputeMeshCookKey's first two terms cover the SAME byte sequence, by
    // construction rather than by two functions agreeing. `sourcePath` is still needed
    // for cgltf_load_buffers' base directory and for diagnostics that name the file.
    //
    // See the REFUSAL LADDER above (this file's banner) for the order applied -- each
    // rung's own spec clause is s4.5. Task 6 landed refusals/warnings only; F2c Task 7
    // fills in `mesh` (flatten, bake, winding flip, sections/slots, remap/optimize, AABB,
    // SourceHash) over the same rungs.
    [[nodiscard]] MeshImportResult ImportMesh(
        std::span<const std::byte> sourceBytes,
        std::span<const std::span<const std::byte>> externalBuffers,
        const std::filesystem::path& sourcePath,
        const Guid& sourceGuid,
        const MeshMetaSettings& settings);

    // Every EXTERNAL buffer this file references, in glTF declaration order, read
    // relative to `sourcePath`. An EMBEDDED buffer (a GLB BIN chunk, a data: URI)
    // contributes NOTHING -- its bytes are already inside `sourceBytes`, and hashing
    // them twice would be harmless but would make "source bytes" mean two things.
    // nullopt when the file does not parse or a referenced buffer is unreadable.
    //
    // SEPARATE from ImportMesh because the cook key must be computable WITHOUT
    // running the importer: CheckProject's staleness probe never imports (CookSession.
    // hpp's "PURE, content-addressed check"), and making it import would turn a
    // read-only --check into a full parse of every mesh in the project.
    [[nodiscard]] std::optional<std::vector<std::vector<std::byte>>> ReadExternalBuffers(
        std::span<const std::byte> sourceBytes, const std::filesystem::path& sourcePath);
}
