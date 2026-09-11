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

    // A glTF image's file stem: its own name when it has one AND that name is
    // "usable" once sanitised, else "<sourceStem>-<index>" (A4's source-derived
    // fallback). Sanitised to filesystem-safe characters, because a glTF name is
    // arbitrary UTF-8 and a '/' in one would silently write outside the intended
    // folder. "Usable" is its own gate on top of that: a name that sanitises down to
    // empty, or to nothing but dots (a bare "." or ".." -- inert against escaping the
    // folder once separators are gone, but still not a name a file can be created
    // under), also falls back to the source-derived name -- treated as "no usable
    // name" exactly like an empty glTF-authored name is.
    [[nodiscard]] std::string ImageFileStem(const std::string& imageName,
                                            const std::string& sourceStem, std::size_t index);

    // A newly-discovered `.gltf`/`.glb`'s embedded images, extracted to loose .png
    // siblings beside `source`. SurveyGltf's front half first (nullopt -- the file
    // will not cook either -- means no extraction, quietly); then, per EMBEDDED image
    // in survey order, a destination is resolved from `source.parent_path()` /
    // (ImageFileStem's stem + the extension `mimeType` implies, "image/png" ->
    // ".png" etc.) by walking a CHAIN of candidates.
    //
    // Spec s5.5 states two invariants that both have to hold at once: an already-
    // extracted .png is NEVER overwritten, and (A4) a name COLLISION is suffixed
    // rather than silently reused. A plain "skip if the destination exists" honors
    // only the first -- it would hand two different sources embedding a same-named
    // image the SAME texture, exactly what A4 exists to prevent. The reconciliation
    // walks the SAME candidate chain UniqueSiblingPath itself walks -- the natural
    // name, then `-1`, `-2`, ... -- applying ONE byte-compare rule at every EXISTING
    // candidate, with no separate "natural" case and "collision" case:
    //   - a candidate that does not exist yet   -> write there. This is reached
    //     immediately (the natural name) when nothing has ever collided, or after
    //     skipping past one or more EXISTING-BUT-DIFFERENT candidates otherwise --
    //     both are the same rule, just reached at a different point in the walk.
    //   - an EXISTING candidate, IDENTICAL bytes -> already extracted right HERE (a
    //     harmless duplicate, or this exact re-extraction already ran) -> the walk
    //     stops, nothing is written.
    //   - an EXISTING candidate, DIFFERENT bytes -> not a match (a user's edit,
    //     another source's own image, or an EARLIER divergence's own sibling) ->
    //     never overwritten; the walk continues to the next suffix.
    // The chain walk (rather than checking the natural name alone) is what keeps a
    // REPEATED divergence from piling up a fresh numbered duplicate on every
    // re-extraction: once a diverged natural name has already produced `stem-1`,
    // re-extracting again matches `stem-1`'s own bytes and stops there, rather than
    // skipping past it (it differs from the still-diverged natural name, which it is
    // never compared against) straight into minting `stem-2`. Re-discovering an
    // entirely unmodified extraction is likewise a true no-op (matches at the natural
    // name itself) -- the pile only grows for an actual NEW divergence, which is the
    // rare case by construction (first discovery, or a genuine edit/collision).
    //
    // Returns only the paths this call actually WROTE, which is what makes a second,
    // all-matched call's empty return the proof that nothing changed.
    [[nodiscard]] std::vector<std::filesystem::path> ExtractEmbeddedTextures(
        const std::filesystem::path& source);
}
