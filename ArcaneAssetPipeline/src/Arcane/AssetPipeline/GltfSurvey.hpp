#pragma once

// Arcane::AssetPipeline::SurveyGltf -- everything the EDITOR needs to know about a
// glTF that is not geometry (F2c Task 13, spec s5.5/s6, A4). Lives in
// ArcaneAssetPipeline so cgltf stays inside ONE library (the 08-21 placement rule);
// the editor links this library already, for CookSession.
//
// WHY THIS EXISTS SEPARATELY FROM ImportMesh. The cook spine is source-file-driven:
// an embedded glTF image has no registered source, no sidecar and no cook key of its
// own, so it cannot ride the ordinary texture-cook path at all (the comparison's
// Decision 10 records why our divergence from UE's in-memory-decode approach is
// forced, not stylistic). The editor's own MeshImportWave.hpp (ArcaneEditor/src/
// Project) answers this by extracting an embedded image to a loose .png SIBLING at
// discovery time, which the ordinary texture path then picks up on its own next
// sweep -- SurveyGltf is the read-only front half that makes that extraction
// possible: it reports every image (with its embedded bytes, when embedded) and
// every material (with the ONE destination this engine has for it -- base color --
// plus everything else the file authored that this engine has nowhere to put).
//
// SurveyGltf runs the SAME parse + validate front half ImportMesh does (rungs 1-4 of
// MeshImporter.hpp's refusal ladder: cgltf_parse, the extensionsRequired gate,
// cgltf_load_buffers, cgltf_validate -- sharing CgltfGuard.hpp's RAII guard rather
// than a second copy), MINUS rung 5's drawability gate, which is mesh-geometry-
// specific and has no bearing on images/materials. nullopt on any of those four
// failing is deliberate: the mesh cook will refuse the same file for the same
// reason, so there is nothing extra worth reporting here.
#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Arcane::AssetPipeline
{
    struct GltfImage
    {
        std::string            name;        // the glTF image or texture name; may be empty
        std::string            mimeType;    // "image/png", "image/jpeg", ...
        std::vector<std::byte> bytes;       // EMBEDDED images only -- empty for external
        bool                   embedded = false;
        std::string            uri;         // external images only, as authored
    };

    struct GltfMaterial
    {
        std::string              name;                 // the SLOT name (R3's key)
        float                    baseColorFactor[4]{ 1.0f, 1.0f, 1.0f, 1.0f };
        int                      baseColorImage = -1;  // index into GltfSurvey::images
        // s6: every PBR input this engine has no destination for, named. ONE warn per
        // file per material is emitted from this list by the editor -- the importer
        // does not log, so a headless arccook run stays silent about editor concerns.
        std::vector<std::string> droppedInputs;
    };

    struct GltfSurvey
    {
        std::vector<GltfImage>    images;
        std::vector<GltfMaterial> materials;
    };

    // Parses (and validates -- same mandatory gate) `sourceBytes` and reports its
    // images and materials. nullopt on any parse/validate failure, which the caller
    // treats as "the cook will refuse this anyway, say nothing extra here".
    [[nodiscard]] std::optional<GltfSurvey> SurveyGltf(
        std::span<const std::byte> sourceBytes, const std::filesystem::path& sourcePath);
}
