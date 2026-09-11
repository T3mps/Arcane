#pragma once

// MeshImportWave -- the editor's PURE helpers for the mesh import wave (F2c Task 13,
// spec s5.5/s6, A4). PURE by design (no ImGui, no Project, no device) so ArcaneTests
// drives them directly -- the AssetPanelModel/CookQueue/ContentDiscovery pattern.
// REMINDER: this .cpp must be added to ArcaneTests' EXPLICIT editor TU list in the
// root premake5.lua (beside ContentDiscovery.cpp), or the test will not link.
//
// A .glb's embedded images have no registered source, no sidecar and no cook key of
// their own, so they cannot ride the ordinary texture-cook path at all -- extraction
// to a loose .png SIBLING (which the ordinary path then picks up on its own next
// discovery sweep) is what makes them cookable. ExtractEmbeddedTextures is the whole
// of that: SurveyGltf (ArcaneAssetPipeline/GltfSurvey.hpp) does the read-only glTF
// parse, this file decides WHERE each embedded image lands on disk and enforces A4's
// never-overwrite rule before writing.

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace Arcane::Editor
{
    // `dir`/`stem``ext`, suffixed "-1", "-2", ... until the name is free on disk.
    // A4's collision rule, shared by extracted textures, minted companions and minted
    // materials -- one definition, because three near-copies is how two of them drift.
    [[nodiscard]] std::filesystem::path UniqueSiblingPath(
        const std::filesystem::path& dir, const std::string& stem, const std::string& ext);

    // A glTF image's file stem: its own name when it has one, else "<sourceStem>-<index>"
    // (A4's source-derived fallback -- a glTF may name no image at all). Sanitised to
    // filesystem-safe characters, because a glTF name is arbitrary UTF-8 and a '/' in
    // one would silently write outside the intended folder.
    [[nodiscard]] std::string ImageFileStem(const std::string& imageName,
                                            const std::string& sourceStem, std::size_t index);

    // A newly-discovered `.gltf`/`.glb`'s embedded images, extracted to loose .png
    // siblings beside `source`. SurveyGltf's front half first (nullopt -- the file
    // will not cook either -- means no extraction, quietly); then, per EMBEDDED image
    // in survey order, the destination is `source.parent_path()` / (ImageFileStem's
    // stem + the extension `mimeType` implies, "image/png" -> ".png" etc.). A4's
    // no-overwrite half is checked BEFORE any write: a destination that already
    // exists -- a user's edited or replaced .png, or another source's own extraction
    // already sitting at that name -- is skipped, never overwritten. Returns only the
    // paths this call actually WROTE, which is what makes a second, no-op call's
    // empty return the proof that nothing was re-extracted.
    [[nodiscard]] std::vector<std::filesystem::path> ExtractEmbeddedTextures(
        const std::filesystem::path& source);
}
