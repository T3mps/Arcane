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
    // in survey order, the NATURAL destination is `source.parent_path()` /
    // (ImageFileStem's stem + the extension `mimeType` implies, "image/png" ->
    // ".png" etc.).
    //
    // Spec s5.5 states two invariants that both have to hold at once: an already-
    // extracted .png is NEVER overwritten, and (A4) a name COLLISION is suffixed
    // rather than silently reused. A plain "skip if the destination exists" honors
    // only the first -- it would hand two different sources embedding a same-named
    // image the SAME texture, exactly what A4 exists to prevent. The reconciliation
    // is a three-way rule, decided by a byte-compare against whatever already sits at
    // the natural destination:
    //   1. nothing there yet                -> write to the natural destination.
    //   2. something there, IDENTICAL bytes -> already extracted (or a harmless
    //                                           duplicate of the same content) ->
    //                                           skip, write nothing.
    //   3. something there, DIFFERENT bytes -> a genuine collision (a user's own
    //                                           edit, or a different source's own
    //                                           image sitting at this name) -> NEVER
    //                                           overwritten; written instead to
    //                                           UniqueSiblingPath's next free name.
    // Byte-compare is what makes both invariants hold together: a user's edited copy
    // is never clobbered (arm 3 gives the freshly-surveyed original its own sibling
    // name instead of touching the edit), and re-discovering the SAME unmodified
    // extraction is a true no-op (arm 2), so repeatedly re-dropping an unchanged
    // source never grows an unbounded pile of "-1", "-2", ... siblings -- that pile
    // only grows for a REAL divergence (arm 3), which is the rare case by
    // construction (first discovery, or an actual re-drop after an edit/collision).
    //
    // Returns only the paths this call actually WROTE (arms 1 and 3), which is what
    // makes a second, all-arm-2 call's empty return the proof that nothing changed.
    [[nodiscard]] std::vector<std::filesystem::path> ExtractEmbeddedTextures(
        const std::filesystem::path& source);
}
